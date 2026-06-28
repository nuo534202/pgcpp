// deadlock.h — Deadlock detection via wait-for graph cycle search.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/proc.c's deadlock
// checker and src/backend/storage/lmgr/deadlock.c.
//
// PG's deadlock detector runs on the wait-for graph: each backend is a node,
// and an edge from A to B means "A is waiting for B to release a lock".
// A cycle in this graph indicates a deadlock.
//
// MyToyDB is single-process and so never deadlocks in practice, but the API
// is preserved (and exercised by tests) so that callers can construct a
// wait-for graph and ask whether a cycle exists.
#pragma once

#include <vector>

namespace mytoydb::storage {

// RegisterWaitFor — record that `waiter` is waiting for `blocker`.
// Replaces any prior edge from `waiter`.
void RegisterWaitFor(int waiter, int blocker);

// UnregisterWaitFor — remove the wait-for edge starting from `waiter`.
void UnregisterWaitFor(int waiter);

// ClearWaitForGraph — drop all wait-for edges (used by tests).
void ClearWaitForGraph();

// WaitForEdges — return all currently-registered (waiter, blocker) edges.
std::vector<std::pair<int, int>> WaitForEdges();

// FindDeadlockCycle — search the wait-for graph for a cycle starting at
// `start`. If a cycle is found, fills *cycle with the node ids in cycle
// order and returns true. Returns false otherwise.
bool FindDeadlockCycle(int start, std::vector<int>* cycle);

// HasDeadlock — true if the wait-for graph contains any cycle.
bool HasDeadlock();

}  // namespace mytoydb::storage
