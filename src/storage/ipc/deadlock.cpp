// deadlock.cpp — Deadlock detection via wait-for graph cycle search.
//
// Converted from PostgreSQL 15's src/backend/storage/lmgr/deadlock.c.
#include "pgcpp/storage/ipc/deadlock.hpp"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <vector>

namespace pgcpp::storage {

namespace {

// WaitGraph — maps waiter → blocker.
std::map<int, int>& WaitGraph() {
    static std::map<int, int> g;
    return g;
}

}  // namespace

void RegisterWaitFor(int waiter, int blocker) {
    WaitGraph()[waiter] = blocker;
}

void UnregisterWaitFor(int waiter) {
    WaitGraph().erase(waiter);
}

void ClearWaitForGraph() {
    WaitGraph().clear();
}

std::vector<std::pair<int, int>> WaitForEdges() {
    std::vector<std::pair<int, int>> result;
    for (const auto& kv : WaitGraph()) {
        result.emplace_back(kv.first, kv.second);
    }
    return result;
}

bool FindDeadlockCycle(int start, std::vector<int>* cycle) {
    cycle->clear();
    std::unordered_set<int> visited;
    std::vector<int> path;
    int current = start;
    while (current != -1) {
        // If we've already visited this node on the current path, found a cycle.
        auto on_path = std::find(path.begin(), path.end(), current);
        if (on_path != path.end()) {
            // Cycle is from on_path to end of path, plus current.
            for (auto it = on_path; it != path.end(); ++it) {
                cycle->push_back(*it);
            }
            cycle->push_back(current);
            return true;
        }
        // If we've already fully explored this node and found no cycle from it,
        // skip it.
        if (visited.count(current) > 0) {
            break;
        }
        visited.insert(current);
        path.push_back(current);
        auto it = WaitGraph().find(current);
        if (it == WaitGraph().end()) {
            break;
        }
        current = it->second;
    }
    return false;
}

bool HasDeadlock() {
    // Check for cycles starting from every node.
    std::unordered_set<int> checked;
    for (const auto& kv : WaitGraph()) {
        if (checked.count(kv.first) > 0) {
            continue;
        }
        std::vector<int> cycle;
        if (FindDeadlockCycle(kv.first, &cycle)) {
            return true;
        }
        // Mark all nodes visited from this start as checked (no cycle through them).
        for (int n : cycle) {
            checked.insert(n);
        }
        // Also mark all nodes we visited without finding a cycle (need to track path).
        // Simpler: just attempt each start node — FindDeadlockCycle is O(n).
        checked.insert(kv.first);
    }
    return false;
}

}  // namespace pgcpp::storage
