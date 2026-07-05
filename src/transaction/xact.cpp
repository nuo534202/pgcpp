// xact.cpp — Transaction state machine implementation.
//
// Converted from PostgreSQL 15's src/backend/access/transam/xact.c.
//
// Implements the transaction lifecycle:
//   - Autocommit: StartTransactionCommand → CommitTransactionCommand
//   - Explicit block: BeginTransactionBlock → ... → EndTransactionBlock
//   - Savepoints: BeginSavepoint → ReleaseSavepoint / RollbackToSavepoint
//
// XID assignment is deferred until the first write (GetCurrentTransactionId
// allocates a new XID on first call within a transaction).
//
// pgcpp simplifications:
//   - Single-process (no distributed commit)
//   - No resource owners (memory contexts handle cleanup)
//   - No GUC nesting
//   - No prepared transactions (2PC)
#include "transaction/xact.hpp"

#include <cstdio>
#include <list>
#include <vector>

#include "access/heapam.hpp"
#include "common/error/elog.hpp"
#include "storage/ipc/proc.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

namespace {

// The transaction state stack. The back element is the current (sub)transaction.
// We use std::list because it guarantees pointer stability on insertion/deletion
// (unlike std::vector, which invalidates pointers on reallocation).
std::list<TransactionStateData>& StateStack() {
    static std::list<TransactionStateData> stack;
    return stack;
}

// The next subtransaction ID to assign.
SubTransactionId& NextSubTransactionId() {
    static SubTransactionId next = kTopSubTransactionId + 1;
    return next;
}

// Get the current (top-of-stack) transaction state, or nullptr if idle.
TransactionState CurrentState() {
    auto& stack = StateStack();
    if (stack.empty())
        return nullptr;
    return &stack.back();
}

// Push a new transaction state onto the stack.
TransactionState PushState() {
    auto& stack = StateStack();
    TransactionStateData state;
    state.nesting_level = static_cast<int>(stack.size()) + 1;
    if (!stack.empty()) {
        state.command_id = stack.back().command_id;
        state.command_id_before_subxact = stack.back().command_id;
    }
    stack.push_back(std::move(state));
    // Set parent pointer AFTER push_back (list pointers are stable, but
    // we need the address of the new back element, not the old one).
    TransactionState s = &stack.back();
    if (stack.size() > 1) {
        auto it = stack.rbegin();
        ++it;  // point to the second-to-last element (the parent)
        s->parent = &(*it);
    }
    return s;
}

// Pop the top state off the stack.
void PopState() {
    auto& stack = StateStack();
    if (!stack.empty()) {
        stack.pop_back();
    }
}

// Start a new top-level transaction.
void StartTransaction() {
    TransactionState s = PushState();
    s->state = TransState::kStart;
    s->block_state = TBlockState::kStarted;
    s->transaction_id = kInvalidTransactionId;  // deferred until first write
    s->sub_transaction_id = kTopSubTransactionId;
    s->command_id = kFirstCommandId;
    s->state = TransState::kInProgress;
}

// Commit the current top-level transaction.
void CommitTransaction() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return;

    s->state = TransState::kCommit;

    // Record the XID as committed in the commit log (if it was assigned).
    if (TransactionIdIsValid(s->transaction_id)) {
        TransactionIdCommit(s->transaction_id);
    }

    // P0-3: clear the XID from PGXACT so concurrent backends no longer see
    // this transaction as running. The PGPROC slot stays registered (the
    // backend is still alive); only the per-transaction XID is cleared.
    pgcpp::storage::PGXACT* pgxact = pgcpp::storage::GetMyPgXact();
    if (pgxact != nullptr) {
        pgxact->xid = pgcpp::transaction::kInvalidTransactionId;
    }

    // Release the transaction snapshot so the next transaction sees the
    // latest committed state.
    ResetTransactionSnapshot();

    PopState();
}

// Abort the current top-level transaction.
void AbortTransaction() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return;

    s->state = TransState::kAbort;

    // Record the XID as aborted in the commit log (if it was assigned).
    if (TransactionIdIsValid(s->transaction_id)) {
        TransactionIdAbort(s->transaction_id);
    }

    // P0-3: clear the XID from PGXACT (same as commit path).
    pgcpp::storage::PGXACT* pgxact = pgcpp::storage::GetMyPgXact();
    if (pgxact != nullptr) {
        pgxact->xid = pgcpp::transaction::kInvalidTransactionId;
    }

    // Release the transaction snapshot.
    ResetTransactionSnapshot();

    PopState();
}

// Start a subtransaction (savepoint).
void StartSubTransaction() {
    TransactionState parent = CurrentState();
    if (parent == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "cannot start subtransaction without a parent transaction");
    }

    TransactionState s = PushState();
    s->state = TransState::kInProgress;
    s->block_state = TBlockState::kSubInProgress;
    s->transaction_id = parent->transaction_id;  // share parent's XID
    s->sub_transaction_id = NextSubTransactionId()++;
    s->command_id = parent->command_id;
    s->command_id_before_subxact = parent->command_id;
}

// Commit a subtransaction.
void CommitSubTransaction() {
    TransactionState s = CurrentState();
    if (s == nullptr || s->parent == nullptr) {
        return;  // nothing to commit
    }

    // Propagate command ID to parent.
    s->parent->command_id = s->command_id;
    PopState();
}

// Abort a subtransaction.
void AbortSubTransaction() {
    TransactionState s = CurrentState();
    if (s == nullptr || s->parent == nullptr) {
        return;  // nothing to abort
    }

    // Restore parent's command ID (discard this subxact's changes).
    s->parent->command_id = s->command_id_before_subxact;
    PopState();
}

}  // namespace

// --- Public API ---

bool IsTransactionBlock() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return false;
    return s->block_state != TBlockState::kDefault && s->block_state != TBlockState::kStarted;
}

bool IsAbortedTransactionBlock() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return false;
    switch (s->block_state) {
        case TBlockState::kAbort:
        case TBlockState::kAbortEnd:
        case TBlockState::kAbortPending:
        case TBlockState::kSubAbort:
        case TBlockState::kSubAbortPending:
        case TBlockState::kSubAbortRestart:
            return true;
        default:
            return false;
    }
}

bool IsTransactionOrTransactionBlock() {
    TransactionState s = CurrentState();
    return s != nullptr && s->state != TransState::kDefault;
}

const char* TransactionBlockStateAsString() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return "default";
    switch (s->block_state) {
        case TBlockState::kDefault:
            return "default";
        case TBlockState::kStarted:
            return "started";
        case TBlockState::kBegin:
            return "begin";
        case TBlockState::kInProgress:
            return "in progress";
        case TBlockState::kEnd:
            return "end";
        case TBlockState::kAbort:
            return "abort";
        case TBlockState::kAbortEnd:
            return "abort end";
        case TBlockState::kAbortPending:
            return "abort pending";
        case TBlockState::kSubBegin:
            return "sub begin";
        case TBlockState::kSubInProgress:
            return "sub in progress";
        case TBlockState::kSubRelease:
            return "sub release";
        case TBlockState::kSubCommit:
            return "sub commit";
        case TBlockState::kSubAbort:
            return "sub abort";
        case TBlockState::kSubAbortPending:
            return "sub abort pending";
        case TBlockState::kSubRestart:
            return "sub restart";
        case TBlockState::kSubAbortRestart:
            return "sub abort restart";
    }
    return "unknown";
}

TransactionId GetCurrentTransactionId() {
    TransactionState s = CurrentState();
    if (s == nullptr) {
        return kInvalidTransactionId;
    }

    // Deferred XID assignment: allocate on first use.
    if (!TransactionIdIsValid(s->transaction_id)) {
        // Walk up to find the top-level transaction.
        TransactionState top = s;
        while (top->parent != nullptr) {
            top = top->parent;
        }
        if (!TransactionIdIsValid(top->transaction_id)) {
            top->transaction_id = AllocateNextTransactionId();
            // P0-3: publish the XID in the PGXACT compact array so concurrent
            // backends see this transaction in their snapshots. The PGPROC slot
            // was registered in ProcArray by InitProcess; we only need to set
            // the xid field here.
            pgcpp::storage::PGXACT* pgxact = pgcpp::storage::GetMyPgXact();
            if (pgxact != nullptr) {
                pgxact->xid = top->transaction_id;
            }
        }
        // Subtransactions share the top-level XID.
        s->transaction_id = top->transaction_id;
    }

    return s->transaction_id;
}

TransactionId GetCurrentTransactionIdIfAny() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return kInvalidTransactionId;
    return s->transaction_id;
}

SubTransactionId GetCurrentSubTransactionId() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return kInvalidSubTransactionId;
    return s->sub_transaction_id;
}

int GetCurrentTransactionNestingLevel() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return 0;
    return s->nesting_level;
}

CommandId GetCurrentCommandId(bool /*used_in_trigger*/) {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return kFirstCommandId;
    return s->command_id;
}

void CommandCounterIncrement() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return;
    // PostgreSQL uses a per-transaction counter that resets on each command.
    // Incrementing allows subsequent scans to see earlier command's changes.
    if (s->command_id < kInvalidCommandId - 1) {
        s->command_id++;
    }
    // Update the active snapshot's curcid so subsequent scans see tuples
    // inserted by the previous command. pgcpp caches the transaction
    // snapshot (GetTransactionSnapshot pushes it once), so we must keep
    // its curcid in sync with the command counter — PostgreSQL sets
    // curcid at snapshot creation but its scans re-evaluate visibility
    // against the live transaction state; here we mirror that effect.
    pgcpp::transaction::Snapshot snap = pgcpp::transaction::GetActiveSnapshot();
    if (snap != nullptr) {
        snap->curcid = s->command_id;
    }
    // Invalidate per-backend visibility caches so subsequent scans see
    // tuples inserted/deleted by the previous command. (PG semantics:
    // a new command can see all changes from prior commands in the
    // same transaction.)
    pgcpp::access::InvalidateAllVisibilityCaches();
}

bool BeginTransactionBlock() {
    TransactionState s = CurrentState();
    if (s == nullptr) {
        // Start a new transaction for the block.
        StartTransaction();
        s = CurrentState();
        if (s == nullptr) {
            ereport(pgcpp::error::LogLevel::kError,
                    "BeginTransactionBlock: failed to start transaction");
        }
        s->block_state = TBlockState::kBegin;
        return true;
    }

    // Already in a transaction — this is a nested BEGIN, which PostgreSQL
    // treats as a warning (the BEGIN is ignored).
    if (s->block_state == TBlockState::kStarted) {
        s->block_state = TBlockState::kInProgress;
        return true;
    }

    // Already in an explicit block — warn and ignore.
    ereport(pgcpp::error::LogLevel::kWarning, "there is already a transaction in progress");
    return false;
}

bool EndTransactionBlock() {
    TransactionState s = CurrentState();
    if (s == nullptr) {
        ereport(pgcpp::error::LogLevel::kWarning, "there is no transaction in progress");
        return false;
    }

    switch (s->block_state) {
        case TBlockState::kStarted:
            // Autocommit single-statement — commit it.
            CommitTransaction();
            return true;

        case TBlockState::kInProgress:
        case TBlockState::kBegin:
            // Normal commit of an explicit block.
            s->block_state = TBlockState::kEnd;
            CommitTransaction();
            return true;

        case TBlockState::kAbort:
        case TBlockState::kAbortEnd:
            // ROLLBACK was already issued — abort and report.
            AbortTransaction();
            return false;

        case TBlockState::kSubInProgress:
        case TBlockState::kSubAbort:
            // COMMIT inside a subtransaction — commit the subxact, then the
            // top-level. PostgreSQL treats this as an error.
            ereport(pgcpp::error::LogLevel::kWarning,
                    "cannot commit a transaction block while a subtransaction is active");
            return false;

        default:
            ereport(pgcpp::error::LogLevel::kWarning,
                    std::string("unexpected state in EndTransactionBlock: ") +
                        TransactionBlockStateAsString());
            return false;
    }
}

void AbortTransactionBlock() {
    TransactionState s = CurrentState();
    if (s == nullptr) {
        ereport(pgcpp::error::LogLevel::kWarning, "there is no transaction in progress");
        return;
    }

    switch (s->block_state) {
        case TBlockState::kStarted:
        case TBlockState::kInProgress:
        case TBlockState::kBegin:
            s->block_state = TBlockState::kAbort;
            AbortTransaction();
            break;

        case TBlockState::kAbort:
        case TBlockState::kAbortEnd:
            // Already aborting — just clean up.
            AbortTransaction();
            break;

        case TBlockState::kSubInProgress:
            // ROLLBACK inside a subtransaction — abort the whole stack.
            while (s != nullptr && s->parent != nullptr) {
                AbortSubTransaction();
                s = CurrentState();
            }
            AbortTransaction();
            break;

        default:
            AbortTransaction();
            break;
    }
}

bool PrepareTransactionBlock(const std::string& /*gid*/) {
    // 2PC not implemented in pgcpp.
    ereport(pgcpp::error::LogLevel::kWarning, "prepared transactions are not supported in pgcpp");
    return false;
}

void StartTransactionCommand() {
    TransactionState s = CurrentState();
    if (s == nullptr) {
        // Autocommit: start a single-statement transaction.
        StartTransaction();
    } else if (s->block_state == TBlockState::kBegin) {
        // First command after BEGIN.
        s->block_state = TBlockState::kInProgress;
    }
}

void CommitTransactionCommand() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return;

    switch (s->block_state) {
        case TBlockState::kStarted:
            // Autocommit — commit the single-statement transaction.
            CommitTransaction();
            break;

        case TBlockState::kInProgress:
            // Inside an explicit block — just increment command counter.
            CommandCounterIncrement();
            break;

        case TBlockState::kEnd:
            CommitTransaction();
            break;

        case TBlockState::kAbort:
        case TBlockState::kAbortEnd:
            AbortTransaction();
            break;

        default:
            // In a subtransaction or other state — increment command counter.
            CommandCounterIncrement();
            break;
    }
}

void AbortCurrentTransaction() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return;

    switch (s->block_state) {
        case TBlockState::kStarted:
            // Autocommit — abort the single-statement transaction.
            AbortTransaction();
            break;

        case TBlockState::kInProgress:
        case TBlockState::kBegin:
            // Error inside an explicit block — mark for abort.
            s->block_state = TBlockState::kAbortPending;
            // Abort the current command but keep the transaction alive
            // (user can issue ROLLBACK or the block will abort on COMMIT).
            break;

        case TBlockState::kSubInProgress:
            // Error inside a subtransaction — abort the subxact.
            s->block_state = TBlockState::kSubAbort;
            AbortSubTransaction();
            break;

        default:
            break;
    }
}

void BeginSavepoint(const std::string& name) {
    TransactionState s = CurrentState();
    if (s == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "SAVEPOINT can only be used in transaction blocks");
    }

    StartSubTransaction();
    s = CurrentState();
    s->name = name;
    s->savepoint_level = s->parent->savepoint_level + 1;
}

void ReleaseSavepoint(const std::string& name) {
    TransactionState s = CurrentState();
    if (s == nullptr || s->parent == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "RELEASE SAVEPOINT can only be used in transaction blocks");
    }

    // Find the savepoint by name (search from innermost to outermost).
    TransactionState target = s;
    while (target != nullptr && target->name != name) {
        target = target->parent;
    }

    if (target == nullptr || target->parent == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "savepoint \"%s\" does not exist", name.c_str());
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }

    // Commit all subtransactions up to and including the target.
    while (CurrentState() != target) {
        CommitSubTransaction();
    }
    CommitSubTransaction();
}

void RollbackToSavepoint(const std::string& name) {
    TransactionState s = CurrentState();
    if (s == nullptr || s->parent == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "ROLLBACK TO SAVEPOINT can only be used in transaction blocks");
    }

    // Find the savepoint by name.
    TransactionState target = s;
    while (target != nullptr && target->name != name) {
        target = target->parent;
    }

    if (target == nullptr || target->parent == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "savepoint \"%s\" does not exist", name.c_str());
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }

    // Abort all subtransactions down to AND INCLUDING the target.
    // We abort until the current state is the target's parent.
    // Save target's parent before the loop: once target itself is popped by
    // AbortSubTransaction(), the `target` pointer becomes dangling.
    TransactionState target_parent = target->parent;
    while (CurrentState() != target_parent) {
        AbortSubTransaction();
    }

    // Start a new subtransaction with the same name (PostgreSQL semantics:
    // ROLLBACK TO leaves the savepoint in place for future use).
    StartSubTransaction();
    CurrentState()->name = name;
}

void InitializeTransactionSystem() {
    auto& stack = StateStack();
    stack.clear();
    NextSubTransactionId() = kTopSubTransactionId + 1;
    InitializeCommitLog();
}

TransactionId GetTopLevelTransactionId() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return kInvalidTransactionId;

    // Walk up to the top-level transaction.
    while (s->parent != nullptr) {
        s = s->parent;
    }

    // Ensure the XID is assigned.
    if (!TransactionIdIsValid(s->transaction_id)) {
        s->transaction_id = AllocateNextTransactionId();
    }
    return s->transaction_id;
}

TransactionId GetTopLevelTransactionIdIfAny() {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return kInvalidTransactionId;

    while (s->parent != nullptr) {
        s = s->parent;
    }
    return s->transaction_id;
}

bool TransactionIdIsCurrentTransactionId(TransactionId xid) {
    TransactionState s = CurrentState();
    if (s == nullptr)
        return false;

    // Check all transactions on the stack (subtransactions share the
    // top-level XID, but we check for completeness).
    while (s != nullptr) {
        if (s->transaction_id == xid)
            return true;
        s = s->parent;
    }

    // Also check if the XID matches the top-level (deferred assignment).
    TransactionId current = GetCurrentTransactionIdIfAny();
    if (current == xid)
        return true;

    return false;
}

}  // namespace pgcpp::transaction
