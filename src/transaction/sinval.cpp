// sinval.cpp — Shared cache invalidation messages.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/sinval.cpp and
// src/backend/storage/ipc/sinvaladt.cpp.
//
// When a backend modifies a catalog table, it sends invalidation messages
// so that other backends invalidate their cached copies. Each message
// identifies what to invalidate (a relation cache entry, a snapshot, a
// catcache entry, an smgr entry, etc.).
//
// In PostgreSQL, messages are queued per-backend and dispatched to all
// backends via the shared invalidation queue (sinvaladt). pgcpp is
// single-process, so we keep a local queue and dispatch to locally
// registered handlers at AcceptInvalidationMessages time. This preserves
// the API and the invalidation-driven cache-refresh pattern.
#include "transaction/sinval.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "catalog/catalog.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

namespace {

// The pending invalidation queue (messages not yet dispatched).
// Implemented as a function-local static to avoid static-initialization-order
// issues (matches the CommitLog() pattern in transam.cpp).
std::vector<SharedInvalidationMessage>& PendingQueue() {
    static std::vector<SharedInvalidationMessage> queue;
    return queue;
}

// A registered invalidation handler with its assigned ID.
struct HandlerEntry {
    int id;
    InvalidHandler handler;
};

// The list of registered invalidation handlers.
std::vector<HandlerEntry>& Handlers() {
    static std::vector<HandlerEntry> handlers;
    return handlers;
}

// The next handler ID to assign. IDs start at 1 so that 0 can be used as a
// sentinel for "no handler" (matches PG's LocalMyBackendId convention).
int& NextHandlerId() {
    static int next = 1;
    return next;
}

}  // namespace

void InitializeSinval() {
    PendingQueue().clear();
    Handlers().clear();
    NextHandlerId() = 1;
}

void ResetSinval() {
    PendingQueue().clear();
    Handlers().clear();
    NextHandlerId() = 1;
}

void CacheInvalidateRelcache(pgcpp::catalog::Oid db_id, pgcpp::catalog::Oid rel_id) {
    SharedInvalidationMessage msg;
    msg.kind = SharedInvalCmdType::kRelcache;
    msg.db_id = db_id;
    msg.rel_id = rel_id;
    PendingQueue().push_back(msg);
}

void CacheInvalidateSnapshot(pgcpp::catalog::Oid db_id) {
    SharedInvalidationMessage msg;
    msg.kind = SharedInvalCmdType::kSnapshot;
    msg.db_id = db_id;
    PendingQueue().push_back(msg);
}

void CacheInvalidateCatcache(pgcpp::catalog::Oid db_id, int cat_id, int hash_value) {
    SharedInvalidationMessage msg;
    msg.kind = SharedInvalCmdType::kCatcache;
    msg.db_id = db_id;
    msg.cat_id = cat_id;
    msg.hash_value = hash_value;
    PendingQueue().push_back(msg);
}

void CacheInvalidateSmgr(pgcpp::catalog::Oid db_id, pgcpp::catalog::Oid rel_id) {
    SharedInvalidationMessage msg;
    msg.kind = SharedInvalCmdType::kSmgr;
    msg.db_id = db_id;
    msg.rel_id = rel_id;
    PendingQueue().push_back(msg);
}

void RecordInvalidationMessage(const SharedInvalidationMessage& msg) {
    PendingQueue().push_back(msg);
}

void SendSharedInvalidationMessages() {
    auto& queue = PendingQueue();
    auto& handlers = Handlers();
    for (const auto& msg : queue) {
        for (const auto& entry : handlers) {
            entry.handler(msg);
        }
    }
    queue.clear();
}

void AcceptInvalidationMessages() {
    SendSharedInvalidationMessages();
}

int RegisterInvalidationHandler(InvalidHandler handler) {
    int id = NextHandlerId();
    NextHandlerId() = id + 1;
    Handlers().push_back({id, std::move(handler)});
    return id;
}

void UnregisterInvalidationHandler(int handler_id) {
    auto& handlers = Handlers();
    auto it =
        std::find_if(handlers.begin(), handlers.end(),
                     [handler_id](const HandlerEntry& entry) { return entry.id == handler_id; });
    if (it != handlers.end()) {
        handlers.erase(it);
    }
}

std::size_t GetPendingInvalidationCount() {
    return PendingQueue().size();
}

}  // namespace pgcpp::transaction
