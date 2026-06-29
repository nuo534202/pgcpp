// sinval.h — Shared cache invalidation messages.
//
// Converted from PostgreSQL 15's src/include/storage/sinval.h and sinvaladt.h.
//
// When a backend modifies a catalog table, it sends invalidation messages
// to all backends so they can invalidate their cached copies. Each message
// identifies what to invalidate (a relation cache entry, a snapshot, a
// catcache entry, a relcache entry, etc.).
//
// pgcpp (single-process) accumulates messages locally and processes them
// immediately at the end of each transaction (AcceptInvalidationMessages).
// This preserves the API and the invalidation-driven cache-refresh pattern.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "catalog/catalog.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

// SharedInvalCmdType — kind of invalidation message (PG's enum).
enum class SharedInvalCmdType : uint8_t {
    kResetSync = 0,
    kCatcache = 1,  // invalidate a syscache/catcache entry
    kRelcache = 2,  // invalidate a relcache (relation) entry
    kSmgr = 3,      // invalidate storage manager cache (drop rel files)
    kSnapshot = 4,  // invalidate all active snapshots
    kRelmap = 5,    // invalidate relation map
};

// SharedInvalidationMessage — one invalidation message (simplified union).
struct SharedInvalidationMessage {
    SharedInvalCmdType kind = SharedInvalCmdType::kResetSync;

    // Union-like fields (PG uses a C union; we keep separate fields).
    pgcpp::catalog::Oid db_id = 0;   // database OID (0 = all databases)
    pgcpp::catalog::Oid rel_id = 0;  // relation OID (for Relcache/Smgr)
    int cat_id = 0;                  // hash value (for Catcache)
    int hash_value = 0;              // hash value (for Catcache)

    SharedInvalidationMessage() = default;
};

// InvalidHandler — callback invoked when AcceptInvalidationMessages processes
// messages. Each message in the queue is passed to all registered handlers.
using InvalidHandler = std::function<void(const SharedInvalidationMessage&)>;

// InitializeSinval — set up the invalidation queue (clear it).
void InitializeSinval();

// ResetSinval — clear the queue and handlers (for testing).
void ResetSinval();

// CacheInvalidateRelcache — enqueue a "invalidate relation cache" message.
void CacheInvalidateRelcache(pgcpp::catalog::Oid db_id, pgcpp::catalog::Oid rel_id);

// CacheInvalidateSnapshot — enqueue a "invalidate snapshots" message.
void CacheInvalidateSnapshot(pgcpp::catalog::Oid db_id);

// CacheInvalidateCatcache — enqueue a "invalidate catcache entry" message.
void CacheInvalidateCatcache(pgcpp::catalog::Oid db_id, int cat_id, int hash_value);

// CacheInvalidateSmgr — enqueue a "invalidate smgr" message (drop rel files).
void CacheInvalidateSmgr(pgcpp::catalog::Oid db_id, pgcpp::catalog::Oid rel_id);

// RecordInvalidationMessage — enqueue a pre-built message.
void RecordInvalidationMessage(const SharedInvalidationMessage& msg);

// SendSharedInvalidationMessages — flush all queued messages to handlers
// (in PG: to all backends). Equivalent to AcceptInvalidationMessages in
// the single-process model: calls all registered handlers for each message,
// then clears the queue.
void SendSharedInvalidationMessages();

// AcceptInvalidationMessages — process pending invalidations.
// Alias for SendSharedInvalidationMessages (PG naming).
void AcceptInvalidationMessages();

// RegisterInvalidationHandler — add a callback to be invoked when messages
// are processed. Returns a handler ID for later removal.
int RegisterInvalidationHandler(InvalidHandler handler);

// UnregisterInvalidationHandler — remove a handler by ID.
void UnregisterInvalidationHandler(int handler_id);

// GetPendingInvalidationCount — number of messages currently queued.
std::size_t GetPendingInvalidationCount();

}  // namespace pgcpp::transaction
