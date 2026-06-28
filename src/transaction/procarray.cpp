// procarray.cpp — Process array: tracks all running transactions.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/procarray.cpp.
//
// ProcArray is the authoritative list of all currently-running backend
// transactions. It is used to compute snapshots (which XIDs are visible),
// determine the oldest running XID (for VACUUM), and build the snapshot of
// running XIDs for standby.
//
// In PostgreSQL, ProcArray is a shared-memory array of PGPROC entries, one
// per backend process. MyToyDB is single-process, so we keep an in-memory
// vector of XIDs that grows/shrinks as transactions start/commit.
#include "mytoydb/transaction/procarray.hpp"

#include <algorithm>
#include <vector>

#include "mytoydb/transaction/transam.hpp"

namespace mytoydb::transaction {

namespace {

// The list of currently-running transactions (XIDs).
// Implemented as a function-local static to avoid static-initialization-order
// issues (matches the CommitLog() pattern in transam.cpp).
std::vector<TransactionId>& ProcArrayData() {
    static std::vector<TransactionId> data;
    return data;
}

}  // namespace

void InitializeProcArray() {
    ProcArrayData().clear();
}

void ResetProcArray() {
    ProcArrayData().clear();
}

void ProcArrayAdd(TransactionId xid) {
    ProcArrayData().push_back(xid);
}

void ProcArrayRemove(TransactionId xid) {
    auto& data = ProcArrayData();
    auto it = std::find(data.begin(), data.end(), xid);
    if (it != data.end()) {
        data.erase(it);
    }
}

TransactionId GetOldestXmin(TransactionId ignore) {
    const auto& data = ProcArrayData();
    TransactionId oldest = kInvalidTransactionId;
    for (TransactionId xid : data) {
        if (ignore != kInvalidTransactionId && xid == ignore) {
            continue;
        }
        if (oldest == kInvalidTransactionId || xid < oldest) {
            oldest = xid;
        }
    }
    if (oldest == kInvalidTransactionId) {
        return kFrozenTransactionId;
    }
    return oldest;
}

std::vector<TransactionId> GetRunningTransactionData() {
    return ProcArrayData();
}

int CountRunningXacts() {
    return static_cast<int>(ProcArrayData().size());
}

bool ProcArrayContains(TransactionId xid) {
    const auto& data = ProcArrayData();
    return std::find(data.begin(), data.end(), xid) != data.end();
}

}  // namespace mytoydb::transaction
