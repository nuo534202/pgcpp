// deadlock.cpp — Deadlock detection via wait-for graph.
//
// Converted from PostgreSQL 15's src/backend/storage/lmgr/deadlock.cpp.
//
// Implements a directed wait-for graph where an edge (A → B) means
// "transaction A is waiting for transaction B to release a lock." A cycle
// in this graph is a deadlock.
//
// The DFS-based cycle detection follows PostgreSQL's approach:
//   - Mark nodes as "visiting" (on the current DFS stack) to detect back edges
//   - When a back edge to the start node is found, extract the cycle
//   - The victim is the youngest transaction (highest XID) in the cycle
#include "transaction/deadlock.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace pgcpp::transaction {

namespace {

// Wait-for graph: adjacency list.
// edges[xid] = list of XIDs that `xid` is waiting for.
std::unordered_map<TransactionId, std::vector<TransactionId>>& WaitGraph() {
    static std::unordered_map<TransactionId, std::vector<TransactionId>> graph;
    return graph;
}

// DFS state for cycle detection.
struct DfsState {
    std::unordered_set<TransactionId> visiting;  // nodes on the current DFS stack
    std::vector<TransactionId> path;             // current DFS path (for cycle extraction)
    TransactionId start_xid;                     // the node we started DFS from
    bool found_cycle;                            // true if a cycle was found
    std::vector<TransactionId> cycle;            // the detected cycle (if any)
};

// DfsVisit — recursive DFS visit for cycle detection.
void DfsVisit(TransactionId node, DfsState& state) {
    state.visiting.insert(node);
    state.path.push_back(node);

    auto it = WaitGraph().find(node);
    if (it != WaitGraph().end()) {
        for (TransactionId neighbor : it->second) {
            if (neighbor == state.start_xid && state.path.size() > 0) {
                // Found a cycle back to the start node.
                state.found_cycle = true;
                state.cycle = state.path;
                return;
            }
            if (state.visiting.find(neighbor) == state.visiting.end()) {
                DfsVisit(neighbor, state);
                if (state.found_cycle) {
                    return;
                }
            }
        }
    }

    state.path.pop_back();
    state.visiting.erase(node);
}

}  // namespace

void AddWaitEdge(TransactionId waiter, TransactionId holder) {
    // Avoid duplicate edges.
    auto& edges = WaitGraph()[waiter];
    if (std::find(edges.begin(), edges.end(), holder) == edges.end()) {
        edges.push_back(holder);
    }
}

void RemoveWaitEdgesFor(TransactionId xid) {
    // Remove all edges where xid is the waiter.
    WaitGraph().erase(xid);

    // Remove all edges where xid is the holder.
    for (auto& [waiter, holders] : WaitGraph()) {
        holders.erase(std::remove(holders.begin(), holders.end(), xid), holders.end());
    }
}

bool DetectDeadlock(TransactionId start_xid) {
    DfsState state;
    state.start_xid = start_xid;
    state.found_cycle = false;
    DfsVisit(start_xid, state);
    return state.found_cycle;
}

std::vector<TransactionId> FindDeadlockCycle(TransactionId start_xid) {
    DfsState state;
    state.start_xid = start_xid;
    state.found_cycle = false;
    DfsVisit(start_xid, state);
    return state.cycle;
}

TransactionId SelectVictim(const std::vector<TransactionId>& cycle) {
    if (cycle.empty()) {
        return kInvalidTransactionId;
    }
    // PostgreSQL picks the youngest transaction (highest XID).
    TransactionId victim = cycle[0];
    for (TransactionId xid : cycle) {
        if (xid > victim) {
            victim = xid;
        }
    }
    return victim;
}

std::vector<std::pair<TransactionId, TransactionId>> GetWaitEdges() {
    std::vector<std::pair<TransactionId, TransactionId>> result;
    for (const auto& [waiter, holders] : WaitGraph()) {
        for (TransactionId holder : holders) {
            result.push_back({waiter, holder});
        }
    }
    return result;
}

std::vector<TransactionId> GetWaitersFor(TransactionId holder) {
    std::vector<TransactionId> waiters;
    for (const auto& [waiter, holders] : WaitGraph()) {
        if (std::find(holders.begin(), holders.end(), holder) != holders.end()) {
            waiters.push_back(waiter);
        }
    }
    return waiters;
}

std::vector<TransactionId> GetHoldersWaitedBy(TransactionId waiter) {
    auto it = WaitGraph().find(waiter);
    if (it == WaitGraph().end()) {
        return {};
    }
    return it->second;
}

void ResetDeadlockDetector() {
    WaitGraph().clear();
}

}  // namespace pgcpp::transaction
