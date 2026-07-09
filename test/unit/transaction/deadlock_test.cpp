// deadlock_test.cpp — Unit tests for the deadlock detector (P1-8).
//
// Tests wait-for graph edge management, DFS-based cycle detection,
// victim selection, and reset semantics.

#include "transaction/deadlock.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include "transaction/transam.hpp"

using pgcpp::transaction::AddWaitEdge;
using pgcpp::transaction::DetectDeadlock;
using pgcpp::transaction::FindDeadlockCycle;
using pgcpp::transaction::GetHoldersWaitedBy;
using pgcpp::transaction::GetWaitEdges;
using pgcpp::transaction::GetWaitersFor;
using pgcpp::transaction::kInvalidTransactionId;
using pgcpp::transaction::RemoveWaitEdgesFor;
using pgcpp::transaction::ResetDeadlockDetector;
using pgcpp::transaction::SelectVictim;
using pgcpp::transaction::TransactionId;

namespace {

class DeadlockTest : public ::testing::Test {
protected:
    void SetUp() override { ResetDeadlockDetector(); }
    void TearDown() override { ResetDeadlockDetector(); }
};

}  // namespace

// --- Edge management ---

TEST_F(DeadlockTest, AddWaitEdgeRecordsEdge) {
    AddWaitEdge(10, 20);
    auto holders = GetHoldersWaitedBy(10);
    ASSERT_EQ(holders.size(), 1u);
    EXPECT_EQ(holders[0], 20u);
}

TEST_F(DeadlockTest, AddWaitEdgeAvoidsDuplicates) {
    AddWaitEdge(10, 20);
    AddWaitEdge(10, 20);  // duplicate
    auto holders = GetHoldersWaitedBy(10);
    EXPECT_EQ(holders.size(), 1u);
}

TEST_F(DeadlockTest, AddWaitEdgeMultipleHolders) {
    AddWaitEdge(10, 20);
    AddWaitEdge(10, 30);
    auto holders = GetHoldersWaitedBy(10);
    EXPECT_EQ(holders.size(), 2u);
}

TEST_F(DeadlockTest, GetWaitersForReturnsWaiters) {
    AddWaitEdge(10, 20);
    AddWaitEdge(30, 20);
    auto waiters = GetWaitersFor(20);
    EXPECT_EQ(waiters.size(), 2u);
}

TEST_F(DeadlockTest, GetWaitEdgesReturnsAll) {
    AddWaitEdge(10, 20);
    AddWaitEdge(30, 40);
    auto edges = GetWaitEdges();
    EXPECT_EQ(edges.size(), 2u);
}

TEST_F(DeadlockTest, RemoveWaitEdgesForRemovesAsWaiter) {
    AddWaitEdge(10, 20);
    AddWaitEdge(10, 30);
    RemoveWaitEdgesFor(10);
    EXPECT_TRUE(GetHoldersWaitedBy(10).empty());
}

TEST_F(DeadlockTest, RemoveWaitEdgesForRemovesAsHolder) {
    AddWaitEdge(10, 20);
    AddWaitEdge(30, 20);
    RemoveWaitEdgesFor(20);
    EXPECT_TRUE(GetHoldersWaitedBy(10).empty());
    EXPECT_TRUE(GetHoldersWaitedBy(30).empty());
}

// --- Cycle detection ---

TEST_F(DeadlockTest, DetectDeadlockNoCycleReturnsFalse) {
    AddWaitEdge(10, 20);  // 10 -> 20 (no back edge)
    EXPECT_FALSE(DetectDeadlock(10));
}

TEST_F(DeadlockTest, DetectDeadlockSimpleCycle) {
    // 10 -> 20 -> 10: classic 2-node deadlock.
    AddWaitEdge(10, 20);
    AddWaitEdge(20, 10);
    EXPECT_TRUE(DetectDeadlock(10));
    EXPECT_TRUE(DetectDeadlock(20));
}

TEST_F(DeadlockTest, DetectDeadlockThreeNodeCycle) {
    // 10 -> 20 -> 30 -> 10
    AddWaitEdge(10, 20);
    AddWaitEdge(20, 30);
    AddWaitEdge(30, 10);
    EXPECT_TRUE(DetectDeadlock(10));
    EXPECT_TRUE(DetectDeadlock(20));
    EXPECT_TRUE(DetectDeadlock(30));
}

TEST_F(DeadlockTest, DetectDeadlockNoBackEdgeFromStart) {
    // 10 -> 20 -> 30 (no cycle back to 10)
    AddWaitEdge(10, 20);
    AddWaitEdge(20, 30);
    EXPECT_FALSE(DetectDeadlock(10));
}

TEST_F(DeadlockTest, DetectDeadlockCycleNotThroughStartReturnsFalse) {
    // 10 -> 20 -> 30 -> 20 (cycle exists but not through 10).
    // Start node 10 is not part of the cycle, so DetectDeadlock(10) returns false.
    AddWaitEdge(10, 20);
    AddWaitEdge(20, 30);
    AddWaitEdge(30, 20);
    EXPECT_FALSE(DetectDeadlock(10));
}

TEST_F(DeadlockTest, FindDeadlockCycleReturnsCycleNodes) {
    // 10 -> 20 -> 30 -> 10
    AddWaitEdge(10, 20);
    AddWaitEdge(20, 30);
    AddWaitEdge(30, 10);
    auto cycle = FindDeadlockCycle(10);
    EXPECT_EQ(cycle.size(), 3u);
    // Cycle should start at 10.
    EXPECT_EQ(cycle[0], 10u);
    // All three nodes should be present.
    EXPECT_NE(std::find(cycle.begin(), cycle.end(), 20u), cycle.end());
    EXPECT_NE(std::find(cycle.begin(), cycle.end(), 30u), cycle.end());
}

TEST_F(DeadlockTest, FindDeadlockCycleEmptyWhenNoCycle) {
    AddWaitEdge(10, 20);
    auto cycle = FindDeadlockCycle(10);
    EXPECT_TRUE(cycle.empty());
}

TEST_F(DeadlockTest, FindDeadlockCycleTwoNodeCycle) {
    AddWaitEdge(10, 20);
    AddWaitEdge(20, 10);
    auto cycle = FindDeadlockCycle(10);
    EXPECT_EQ(cycle.size(), 2u);
    EXPECT_EQ(cycle[0], 10u);
    EXPECT_EQ(cycle[1], 20u);
}

// --- Victim selection ---

TEST_F(DeadlockTest, SelectVictimReturnsHighestXid) {
    std::vector<TransactionId> cycle = {10, 30, 20};
    EXPECT_EQ(SelectVictim(cycle), 30u);
}

TEST_F(DeadlockTest, SelectVictimSingleElement) {
    std::vector<TransactionId> cycle = {42};
    EXPECT_EQ(SelectVictim(cycle), 42u);
}

TEST_F(DeadlockTest, SelectVictimEmptyReturnsInvalid) {
    std::vector<TransactionId> cycle;
    EXPECT_EQ(SelectVictim(cycle), kInvalidTransactionId);
}

// --- Reset ---

TEST_F(DeadlockTest, ResetClearsAllEdges) {
    AddWaitEdge(10, 20);
    AddWaitEdge(30, 40);
    ResetDeadlockDetector();
    EXPECT_TRUE(GetWaitEdges().empty());
    EXPECT_TRUE(GetHoldersWaitedBy(10).empty());
}

// --- End-to-end scenario ---

TEST_F(DeadlockTest, EndToEndDeadlockResolution) {
    // Simulate: T1 holds lock A, waits for B.
    //           T2 holds lock B, waits for C.
    //           T3 holds lock C, waits for A.
    // Cycle: T1 -> T2 -> T3 -> T1.
    TransactionId t1 = 100, t2 = 200, t3 = 300;
    AddWaitEdge(t1, t2);
    AddWaitEdge(t2, t3);
    AddWaitEdge(t3, t1);

    EXPECT_TRUE(DetectDeadlock(t1));
    auto cycle = FindDeadlockCycle(t1);
    ASSERT_FALSE(cycle.empty());

    TransactionId victim = SelectVictim(cycle);
    EXPECT_EQ(victim, t3);  // youngest = highest XID

    // Abort the victim: remove all its edges.
    RemoveWaitEdgesFor(victim);
    EXPECT_FALSE(DetectDeadlock(t1));
}
