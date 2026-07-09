// deadlock.h — Deadlock detection via wait-for graph.
//
// Converted from PostgreSQL 15's src/backend/storage/lmgr/deadlock.cpp.
//
// A wait-for graph records which transaction is waiting for which other
// transaction (because the holder owns a lock the waiter needs). A cycle in
// this graph means deadlock: each transaction in the cycle is waiting for
// the next, so none can ever proceed.
//
// pgcpp is single-process, so real deadlocks cannot occur at runtime.
// However, the algorithm is implemented faithfully so that:
//   - Multi-transaction scenarios can be simulated and tested
//   - The architecture is ready for future multi-process support
//   - The educational value of PG's deadlock checker is preserved
//
// Algorithm (DFS-based cycle detection):
//   1. When a transaction T waits for a lock, add edges T → H for each
//      holder H of the conflicting lock.
//   2. Run DFS from T, searching for a path back to T.
//   3. If a cycle is found, select a victim (the youngest XID in the cycle)
//      and abort it to break the deadlock.
#pragma once

#include <cstdint>
#include <vector>

#include "transaction/transam.hpp"

namespace pgcpp::transaction {

// AddWaitEdge — record that `waiter` is waiting for `holder`.
// Multiple edges can exist for the same waiter (waiting for multiple holders).
void AddWaitEdge(TransactionId waiter, TransactionId holder);

// RemoveWaitEdgesFor — remove all wait edges involving `xid` (as waiter or
// holder). Called when a transaction completes (commit/abort) and its
// locks are released.
void RemoveWaitEdgesFor(TransactionId xid);

// DetectDeadlock — check if there's a cycle in the wait-for graph starting
// from `start_xid`. Returns true if a cycle is found.
// Uses DFS with a "visiting" (gray) set to detect back edges.
bool DetectDeadlock(TransactionId start_xid);

// FindDeadlockCycle — find the cycle containing `start_xid`.
// Returns the list of XIDs in the cycle (empty if no cycle found).
// The cycle is ordered: cycle[0] waits for cycle[1], ..., cycle[n-1] waits
// for cycle[0].
std::vector<TransactionId> FindDeadlockCycle(TransactionId start_xid);

// SelectVictim — given a deadlock cycle, select the transaction to abort.
// PostgreSQL picks the youngest transaction (highest XID) to minimize
// wasted work. Returns the victim XID.
TransactionId SelectVictim(const std::vector<TransactionId>& cycle);

// GetWaitEdges — return all current wait edges (for testing/inspection).
// Each pair is (waiter, holder).
std::vector<std::pair<TransactionId, TransactionId>> GetWaitEdges();

// GetWaitersFor — return all transactions waiting for `holder`.
std::vector<TransactionId> GetWaitersFor(TransactionId holder);

// GetHoldersWaitedBy — return all transactions that `waiter` is waiting for.
std::vector<TransactionId> GetHoldersWaitedBy(TransactionId waiter);

// ResetDeadlockDetector — clear all wait edges (for testing).
void ResetDeadlockDetector();

}  // namespace pgcpp::transaction
