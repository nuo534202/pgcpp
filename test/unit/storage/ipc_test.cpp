// ipc_test.cpp — Unit tests for Task 15.18 (M6 shared buffers + IPC).
//
// Tests the in-process simulations of the IPC/FSM/sync/large-object
// subsystems:
//   - latch (WaitEventSet, WaitLatch)
//   - shmem (named region map)
//   - shmqueue (intrusive doubly-linked list)
//   - shm_mq (ring buffer message queue)
//   - ipci (init-function registry)
//   - proc (PGPROC array)
//   - lwlock (reader/writer locks)
//   - predicate (SERIALIZABLE predicate locks)
//   - deadlock (wait-for graph cycle detection)
//   - fd (VFD cache + real file I/O)
//   - sync (fsync queue)
//   - freespace (per-relation free space map)
//   - fsmpage (FSM leaf page binary tree)
//   - large_object (inv_api read/write/seek/truncate)

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pgcpp/storage/block.hpp"
#include "pgcpp/storage/file/fd.hpp"
#include "pgcpp/storage/freespace/freespace.hpp"
#include "pgcpp/storage/freespace/fsmpage.hpp"
#include "pgcpp/storage/ipc/deadlock.hpp"
#include "pgcpp/storage/ipc/ipci.hpp"
#include "pgcpp/storage/ipc/latch.hpp"
#include "pgcpp/storage/ipc/lwlock.hpp"
#include "pgcpp/storage/ipc/predicate.hpp"
#include "pgcpp/storage/ipc/proc.hpp"
#include "pgcpp/storage/ipc/shm_mq.hpp"
#include "pgcpp/storage/ipc/shmem.hpp"
#include "pgcpp/storage/ipc/shmqueue.hpp"
#include "pgcpp/storage/large_object/inv_api.hpp"
#include "pgcpp/storage/relfilenode.hpp"
#include "pgcpp/storage/sync/sync.hpp"

using pgcpp::storage::AllocateFile;
using pgcpp::storage::AllProcs;
using pgcpp::storage::BytesForFreeSpaceCategory;
using pgcpp::storage::ClearWaitForGraph;
using pgcpp::storage::CloseTransientFiles;
using pgcpp::storage::CreateSharedMemoryAndSemaphores;
using pgcpp::storage::File;
using pgcpp::storage::FileClose;
using pgcpp::storage::FileName;
using pgcpp::storage::FileRead;
using pgcpp::storage::FileSeek;
using pgcpp::storage::FileSync;
using pgcpp::storage::FileTruncate;
using pgcpp::storage::FileWrite;
using pgcpp::storage::FindDeadlockCycle;
using pgcpp::storage::FreeFile;
using pgcpp::storage::FreeSpaceCategoryForBytes;
using pgcpp::storage::FreeSpaceMapDropRel;
using pgcpp::storage::FreeSpaceMapTruncateRel;
using pgcpp::storage::FreeSpaceMapVacuumRel;
using pgcpp::storage::FSMPageClear;
using pgcpp::storage::FSMPageData;
using pgcpp::storage::FSMPageGetFreeSpace;
using pgcpp::storage::FSMPageGetSlot;
using pgcpp::storage::FSMPageInit;
using pgcpp::storage::FSMPageSearchFreeSpace;
using pgcpp::storage::FSMPageSetFreeSpace;
using pgcpp::storage::GetFreeSpaceMapForRel;
using pgcpp::storage::GetLargeObject;
using pgcpp::storage::GetMyProc;
using pgcpp::storage::GetPageWithFreeSpace;
using pgcpp::storage::GetPendingSyncRequests;
using pgcpp::storage::GetPredicateLocks;
using pgcpp::storage::GetSyncStats;
using pgcpp::storage::HasDeadlock;
using pgcpp::storage::HeldLWLockIds;
using pgcpp::storage::InitFileAccess;
using pgcpp::storage::InitializeAllLWLocks;
using pgcpp::storage::InitLatch;
using pgcpp::storage::InitProcess;
using pgcpp::storage::inv_close;
using pgcpp::storage::inv_create;
using pgcpp::storage::inv_drop;
using pgcpp::storage::inv_open;
using pgcpp::storage::inv_read;
using pgcpp::storage::inv_seek;
using pgcpp::storage::inv_tell;
using pgcpp::storage::inv_truncate;
using pgcpp::storage::inv_write;
using pgcpp::storage::kInvalidBlockNumber;
using pgcpp::storage::kInvalidFile;
using pgcpp::storage::kInvalidLargeObjectOid;
using pgcpp::storage::kInvRdwr;
using pgcpp::storage::kInvWrite;
using pgcpp::storage::kOAppend;
using pgcpp::storage::kOCreate;
using pgcpp::storage::kOExclusive;
using pgcpp::storage::kOReadOnly;
using pgcpp::storage::kOReadWrite;
using pgcpp::storage::kWaitLatchSet;
using pgcpp::storage::kWaitTimeout;
using pgcpp::storage::LargeObject;
using pgcpp::storage::LargeObjectDesc;
using pgcpp::storage::LargeObjectOid;
using pgcpp::storage::Latch;
using pgcpp::storage::LookupNamedLock;
using pgcpp::storage::LWLock;
using pgcpp::storage::LWLockAcquire;
using pgcpp::storage::LWLockConditionalAcquire;
using pgcpp::storage::LWLockHeldByMe;
using pgcpp::storage::LWLockHeldByMeInMode;
using pgcpp::storage::LWLockId;
using pgcpp::storage::LWLockInitialize;
using pgcpp::storage::LWLockMode;
using pgcpp::storage::LWLockRelease;
using pgcpp::storage::NumHeldLWLocks;
using pgcpp::storage::NumLargeObjects;
using pgcpp::storage::NumOpenFiles;
using pgcpp::storage::NumPendingSyncRequests;
using pgcpp::storage::NumPredicateLocks;
using pgcpp::storage::NumProcs;
using pgcpp::storage::NumShmemRegions;
using pgcpp::storage::PathNameOpenFile;
using pgcpp::storage::PGPROC;
using pgcpp::storage::PredicateLockConflicts;
using pgcpp::storage::PredicateLockPage;
using pgcpp::storage::PredicateLockRelation;
using pgcpp::storage::PredicateLockRelease;
using pgcpp::storage::PredicateLockReleaseAll;
using pgcpp::storage::PredicateLockTuple;
using pgcpp::storage::ProcGlobalReset;
using pgcpp::storage::RecordPageWithFreeSpace;
using pgcpp::storage::RegisteredIPCInitFns;
using pgcpp::storage::RegisterIPCInitFn;
using pgcpp::storage::RegisterSyncRequest;
using pgcpp::storage::RegisterWaitFor;
using pgcpp::storage::RelFileNode;
using pgcpp::storage::ResetAllLWLocks;
using pgcpp::storage::ResetFreeSpaceMap;
using pgcpp::storage::ResetIPCInitFns;
using pgcpp::storage::ResetLargeObjects;
using pgcpp::storage::ResetLatch;
using pgcpp::storage::ResetShmem;
using pgcpp::storage::ResetSyncQueue;
using pgcpp::storage::ResetVfdCache;
using pgcpp::storage::SetLatch;
using pgcpp::storage::SetMyProc;
using pgcpp::storage::ShmemInitStruct;
using pgcpp::storage::ShmemSize;
using pgcpp::storage::ShmMq;
using pgcpp::storage::ShmMqCreate;
using pgcpp::storage::ShmMqDestroy;
using pgcpp::storage::ShmMqResult;
using pgcpp::storage::SHMQueue;
using pgcpp::storage::SHMQueueDelete;
using pgcpp::storage::SHMQueueElem;
using pgcpp::storage::SHMQueueEmpty;
using pgcpp::storage::SHMQueueInit;
using pgcpp::storage::SHMQueueLength;
using pgcpp::storage::SHMQueuePop;
using pgcpp::storage::SHMQueuePush;
using pgcpp::storage::SHMQueuePushTail;
using pgcpp::storage::SyncFile;
using pgcpp::storage::SyncRequest;
using pgcpp::storage::SyncRequestTag;
using pgcpp::storage::SyncStats;
using pgcpp::storage::TestLatch;
using pgcpp::storage::UnregisterWaitFor;
using pgcpp::storage::WaitEventSet;
using pgcpp::storage::WaitForEdges;
using pgcpp::storage::WaitLatch;

namespace {
constexpr char kTmpFile[] = "/tmp/pgcpp_ipc_test.tmp";
}  // namespace

// Fixture: resets all subsystems between tests so the suite is order-independent.
class IpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetShmem();
        ResetIPCInitFns();
        ProcGlobalReset();
        ResetAllLWLocks();
        PredicateLockReleaseAll();
        ClearWaitForGraph();
        ResetVfdCache();
        InitFileAccess();
        ResetSyncQueue();
        ResetFreeSpaceMap();
        ResetLargeObjects();
        // Remove any leftover temp file from a prior run.
        std::remove(kTmpFile);
    }
    void TearDown() override { std::remove(kTmpFile); }
};

// =============================================================================
// latch tests
// =============================================================================

TEST_F(IpcTest, LatchInitStartsUnset) {
    Latch latch;
    InitLatch(&latch);
    EXPECT_FALSE(TestLatch(&latch));
}

TEST_F(IpcTest, LatchSetReturnsTrueOnTransition) {
    Latch latch;
    InitLatch(&latch);
    EXPECT_TRUE(SetLatch(&latch));   // first set returns true (was unset)
    EXPECT_FALSE(SetLatch(&latch));  // second set returns false (already set)
    EXPECT_TRUE(TestLatch(&latch));
}

TEST_F(IpcTest, LatchResetClearsSetFlag) {
    Latch latch;
    InitLatch(&latch);
    SetLatch(&latch);
    ResetLatch(&latch);
    EXPECT_FALSE(TestLatch(&latch));
}

TEST_F(IpcTest, WaitLatchReturnsImmediatelyIfSet) {
    Latch latch;
    InitLatch(&latch);
    SetLatch(&latch);
    uint32_t result = WaitLatch(&latch, /*timeout_ms=*/100, /*wait_event_mask=*/0);
    EXPECT_EQ(result, kWaitLatchSet);
}

TEST_F(IpcTest, WaitLatchTimesOutIfUnset) {
    Latch latch;
    InitLatch(&latch);
    uint32_t result = WaitLatch(&latch, /*timeout_ms=*/20, /*wait_event_mask=*/0);
    EXPECT_EQ(result, kWaitTimeout);
}

TEST_F(IpcTest, WaitEventSetAcceptsLatchAndSocketEvents) {
    WaitEventSet set;
    Latch latch;
    InitLatch(&latch);
    set.AddLatch(&latch);
    set.AddSocket(/*fd=*/-1, pgcpp::storage::kWaitSocketReadable);
    EXPECT_EQ(set.NumEvents(), 2u);
    set.Clear();
    EXPECT_EQ(set.NumEvents(), 0u);
}

TEST_F(IpcTest, WaitEventSetWaitsForLatch) {
    WaitEventSet set;
    Latch latch;
    InitLatch(&latch);
    set.AddLatch(&latch);
    SetLatch(&latch);
    std::vector<pgcpp::storage::WaitEvent> occurred;
    int n = pgcpp::storage::WaitEventSetWait(&set, /*timeout_ms=*/100, &occurred);
    EXPECT_GE(n, 1);
    EXPECT_FALSE(occurred.empty());
}

// =============================================================================
// shmem tests
// =============================================================================

TEST_F(IpcTest, ShmemInitStructCreatesNewRegion) {
    bool found = true;
    void* p = ShmemInitStruct("region1", 100, &found);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(found);
    EXPECT_EQ(NumShmemRegions(), 1);
    EXPECT_EQ(ShmemSize(), 100u);
}

TEST_F(IpcTest, ShmemInitStructReturnsExistingRegion) {
    bool found = false;
    void* p1 = ShmemInitStruct("region1", 100, &found);
    ASSERT_NE(p1, nullptr);
    EXPECT_FALSE(found);
    void* p2 = ShmemInitStruct("region1", 100, &found);
    ASSERT_NE(p2, nullptr);
    EXPECT_TRUE(found);
    EXPECT_EQ(p1, p2);                // same address
    EXPECT_EQ(NumShmemRegions(), 1);  // not duplicated
}

TEST_F(IpcTest, ShmemInitStructResizesExistingRegion) {
    bool found = false;
    ShmemInitStruct("region1", 50, &found);
    EXPECT_FALSE(found);
    found = false;
    ShmemInitStruct("region1", 200, &found);
    EXPECT_TRUE(found);
    EXPECT_EQ(ShmemSize(), 200u);
}

TEST_F(IpcTest, ResetShmemClearsAllRegions) {
    ShmemInitStruct("a", 16, nullptr);
    ShmemInitStruct("b", 32, nullptr);
    EXPECT_EQ(NumShmemRegions(), 2);
    ResetShmem();
    EXPECT_EQ(NumShmemRegions(), 0);
    EXPECT_EQ(ShmemSize(), 0u);
}

TEST_F(IpcTest, CreateSharedMemoryAndSemaphoresInitsIndex) {
    CreateSharedMemoryAndSemaphores();
    EXPECT_EQ(NumShmemRegions(), 0);  // index exists but no regions yet
}

// =============================================================================
// shmqueue tests
// =============================================================================

TEST_F(IpcTest, ShmQueueInitIsEmpty) {
    SHMQueue q;
    SHMQueueInit(&q);
    EXPECT_TRUE(SHMQueueEmpty(&q));
    EXPECT_EQ(SHMQueueLength(&q), 0);
}

TEST_F(IpcTest, ShmQueuePushAndPopHead) {
    SHMQueue q;
    SHMQueueInit(&q);
    SHMQueueElem a, b;
    SHMQueuePush(&q, &a);
    SHMQueuePush(&q, &b);  // b is now at head, a is at tail
    EXPECT_EQ(SHMQueueLength(&q), 2);
    EXPECT_EQ(SHMQueuePop(&q), &b);
    EXPECT_EQ(SHMQueuePop(&q), &a);
    EXPECT_EQ(SHMQueuePop(&q), nullptr);
    EXPECT_TRUE(SHMQueueEmpty(&q));
}

TEST_F(IpcTest, ShmQueuePushTailPreservesFifoOrder) {
    SHMQueue q;
    SHMQueueInit(&q);
    SHMQueueElem a, b, c;
    SHMQueuePushTail(&q, &a);
    SHMQueuePushTail(&q, &b);
    SHMQueuePushTail(&q, &c);
    EXPECT_EQ(SHMQueuePop(&q), &a);
    EXPECT_EQ(SHMQueuePop(&q), &b);
    EXPECT_EQ(SHMQueuePop(&q), &c);
}

TEST_F(IpcTest, ShmQueueDeleteUnlinksElement) {
    SHMQueue q;
    SHMQueueInit(&q);
    SHMQueueElem a, b, c;
    SHMQueuePushTail(&q, &a);
    SHMQueuePushTail(&q, &b);
    SHMQueuePushTail(&q, &c);
    SHMQueueDelete(&b);  // remove middle
    EXPECT_EQ(SHMQueueLength(&q), 2);
    EXPECT_EQ(SHMQueuePop(&q), &a);
    EXPECT_EQ(SHMQueuePop(&q), &c);
}

// =============================================================================
// shm_mq tests
// =============================================================================

TEST_F(IpcTest, ShmMqSendAndReceiveRoundtrips) {
    auto* mq = ShmMqCreate(1024);
    ASSERT_NE(mq, nullptr);
    EXPECT_TRUE(mq->IsEmpty());
    const uint8_t data[] = {1, 2, 3, 4, 5};
    EXPECT_EQ(mq->Send(data, sizeof(data), /*non_blocking=*/true), ShmMqResult::kSuccess);
    EXPECT_EQ(mq->Bytes(), sizeof(data));
    uint8_t buf[16] = {0};
    std::size_t received = 0;
    EXPECT_EQ(mq->Receive(buf, sizeof(buf), &received, /*non_blocking=*/true),
              ShmMqResult::kSuccess);
    EXPECT_EQ(received, sizeof(data));
    EXPECT_EQ(std::memcmp(buf, data, sizeof(data)), 0);
    ShmMqDestroy(mq);
}

TEST_F(IpcTest, ShmMqWrapsAroundBuffer) {
    auto* mq = ShmMqCreate(8);
    ASSERT_NE(mq, nullptr);
    // Send 6 bytes, receive 6 (write_pos advances near end).
    const uint8_t a[] = {1, 2, 3, 4, 5, 6};
    EXPECT_EQ(mq->Send(a, sizeof(a), false), ShmMqResult::kSuccess);
    uint8_t buf[8] = {0};
    std::size_t got = 0;
    mq->Receive(buf, 6, &got, false);
    EXPECT_EQ(got, 6u);
    // Now send 4 bytes — must wrap around to start of buffer.
    const uint8_t b[] = {10, 11, 12, 13};
    EXPECT_EQ(mq->Send(b, sizeof(b), false), ShmMqResult::kSuccess);
    std::memset(buf, 0, sizeof(buf));
    got = 0;
    mq->Receive(buf, 4, &got, false);
    EXPECT_EQ(got, 4u);
    EXPECT_EQ(buf[0], 10);
    EXPECT_EQ(buf[3], 13);
    ShmMqDestroy(mq);
}

TEST_F(IpcTest, ShmMqNonBlockingReturnsWouldBlockWhenFull) {
    auto* mq = ShmMqCreate(4);
    ASSERT_NE(mq, nullptr);
    const uint8_t data[] = {1, 2, 3, 4};
    EXPECT_EQ(mq->Send(data, sizeof(data), true), ShmMqResult::kSuccess);
    EXPECT_EQ(mq->Send(data, 1, true), ShmMqResult::kWouldBlock);
    ShmMqDestroy(mq);
}

TEST_F(IpcTest, ShmMqNonBlockingReceiveReturnsWouldBlockWhenEmpty) {
    auto* mq = ShmMqCreate(16);
    ASSERT_NE(mq, nullptr);
    uint8_t buf[8] = {0};
    std::size_t got = 99;
    EXPECT_EQ(mq->Receive(buf, sizeof(buf), &got, true), ShmMqResult::kWouldBlock);
    EXPECT_EQ(got, 0u);
    ShmMqDestroy(mq);
}

// =============================================================================
// ipci tests
// =============================================================================

TEST_F(IpcTest, RegisterIPCInitFnIsIdempotent) {
    int count = 0;
    auto fn = [&count]() {
        count++;
        return true;
    };
    RegisterIPCInitFn("test", fn);
    RegisterIPCInitFn("test", fn);  // replaces, doesn't duplicate
    EXPECT_EQ(RegisteredIPCInitFns().size(), 1u);
}

TEST_F(IpcTest, CreateSharedMemoryAndSemaphoresDispatchesAllFns) {
    int called_a = 0, called_b = 0;
    RegisterIPCInitFn("a", [&called_a]() {
        called_a++;
        return true;
    });
    RegisterIPCInitFn("b", [&called_b]() {
        called_b++;
        return true;
    });
    int succeeded = CreateSharedMemoryAndSemaphores();
    EXPECT_EQ(succeeded, 2);
    EXPECT_EQ(called_a, 1);
    EXPECT_EQ(called_b, 1);
}

TEST_F(IpcTest, FailedInitFnDoesNotIncrementSuccess) {
    RegisterIPCInitFn("good", []() { return true; });
    RegisterIPCInitFn("bad", []() { return false; });
    int succeeded = CreateSharedMemoryAndSemaphores();
    EXPECT_EQ(succeeded, 1);
}

// =============================================================================
// proc tests
// =============================================================================

TEST_F(IpcTest, InitProcessCreatesMyProc) {
    PGPROC* p = InitProcess();
    ASSERT_NE(p, nullptr);
    EXPECT_GT(p->pid, 0);
    EXPECT_EQ(GetMyProc(), p);
}

TEST_F(IpcTest, InitProcessAssignsUniqueLxids) {
    PGPROC* p1 = InitProcess();
    PGPROC* p2 = InitProcess();
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1->lxid, p2->lxid);
    EXPECT_EQ(NumProcs(), 2);
}

TEST_F(IpcTest, AllProcsReturnsEveryProc) {
    InitProcess();
    InitProcess();
    auto procs = AllProcs();
    EXPECT_EQ(procs.size(), 2u);
}

TEST_F(IpcTest, SetMyProcOverridesMyProcPointer) {
    PGPROC proc;
    proc.backend_id = "test";
    SetMyProc(&proc);
    EXPECT_EQ(GetMyProc(), &proc);
    EXPECT_EQ(GetMyProc()->backend_id, "test");
}

// =============================================================================
// lwlock tests
// =============================================================================

TEST_F(IpcTest, LwLockExclusiveAcquireRelease) {
    LWLock lock;
    LWLockInitialize(&lock, LWLockId::kShmemIndexLock);
    EXPECT_TRUE(LWLockAcquire(&lock, LWLockMode::kExclusive));
    EXPECT_TRUE(LWLockHeldByMe(&lock));
    EXPECT_TRUE(LWLockHeldByMeInMode(&lock, LWLockMode::kExclusive));
    LWLockRelease(&lock);
    EXPECT_FALSE(LWLockHeldByMe(&lock));
    EXPECT_EQ(NumHeldLWLocks(), 0);
}

TEST_F(IpcTest, LwLockSharedAcquireRelease) {
    LWLock lock;
    LWLockInitialize(&lock, LWLockId::kProcArrayLock);
    EXPECT_TRUE(LWLockAcquire(&lock, LWLockMode::kShared));
    EXPECT_TRUE(LWLockHeldByMeInMode(&lock, LWLockMode::kShared));
    LWLockRelease(&lock);
    EXPECT_FALSE(LWLockHeldByMe(&lock));
}

TEST_F(IpcTest, LwLockConditionalAcquireFailsIfExclusiveHeld) {
    LWLock lock;
    LWLockInitialize(&lock, LWLockId::kShmemIndexLock);
    EXPECT_TRUE(LWLockAcquire(&lock, LWLockMode::kExclusive));
    EXPECT_FALSE(LWLockConditionalAcquire(&lock, LWLockMode::kShared));
    LWLockRelease(&lock);
    EXPECT_TRUE(LWLockConditionalAcquire(&lock, LWLockMode::kShared));
    LWLockRelease(&lock);
}

TEST_F(IpcTest, LookupNamedLockReturnsStablePointer) {
    InitializeAllLWLocks();
    LWLock* p1 = LookupNamedLock(LWLockId::kShmemIndexLock);
    LWLock* p2 = LookupNamedLock(LWLockId::kShmemIndexLock);
    EXPECT_EQ(p1, p2);
    EXPECT_TRUE(LWLockAcquire(p1, LWLockMode::kExclusive));
    LWLockRelease(p1);
}

TEST_F(IpcTest, HeldLWLockIdsReturnsAllHeld) {
    LWLock* a = LookupNamedLock(LWLockId::kOidGenLock);
    LWLock* b = LookupNamedLock(LWLockId::kXactGenLock);
    LWLockAcquire(a, LWLockMode::kShared);
    LWLockAcquire(b, LWLockMode::kExclusive);
    auto ids = HeldLWLockIds();
    EXPECT_EQ(ids.size(), 2u);
    LWLockRelease(a);
    LWLockRelease(b);
}

// =============================================================================
// predicate tests
// =============================================================================

TEST_F(IpcTest, PredicateLockTupleLocksTuple) {
    RelFileNode rnode{1, 100, 1000};
    PredicateLockTuple(rnode, /*block_num=*/5, /*offset_num=*/10, /*xid=*/42);
    EXPECT_EQ(NumPredicateLocks(), 1);
    EXPECT_TRUE(PredicateLockConflicts(rnode, 5, 10));
    EXPECT_FALSE(PredicateLockConflicts(rnode, 5, 11));  // different tuple
}

TEST_F(IpcTest, PredicateLockPageCoversAllTuplesOnPage) {
    RelFileNode rnode{1, 100, 1000};
    PredicateLockPage(rnode, /*block_num=*/5, /*xid=*/42);
    EXPECT_TRUE(PredicateLockConflicts(rnode, 5, 1));
    EXPECT_TRUE(PredicateLockConflicts(rnode, 5, 999));
    EXPECT_FALSE(PredicateLockConflicts(rnode, 6, 1));  // different page
}

TEST_F(IpcTest, PredicateLockRelationCoversAllPagesAndTuples) {
    RelFileNode rnode{1, 100, 1000};
    PredicateLockRelation(rnode, /*xid=*/42);
    EXPECT_TRUE(PredicateLockConflicts(rnode, 100, 1));
    EXPECT_TRUE(PredicateLockConflicts(rnode, 99999, 99));
}

TEST_F(IpcTest, PredicateLockReleaseDropsOnlyXidLocks) {
    RelFileNode rnode{1, 100, 1000};
    PredicateLockTuple(rnode, 5, 1, /*xid=*/42);
    PredicateLockTuple(rnode, 5, 2, /*xid=*/43);
    EXPECT_EQ(PredicateLockRelease(42), 1);
    EXPECT_EQ(NumPredicateLocks(), 1);
    EXPECT_TRUE(PredicateLockConflicts(rnode, 5, 2));
    EXPECT_FALSE(PredicateLockConflicts(rnode, 5, 1));
}

TEST_F(IpcTest, PredicateLockTupleDedup) {
    RelFileNode rnode{1, 100, 1000};
    PredicateLockTuple(rnode, 5, 1, 42);
    PredicateLockTuple(rnode, 5, 1, 42);  // duplicate
    EXPECT_EQ(NumPredicateLocks(), 1);
}

// =============================================================================
// deadlock tests
// =============================================================================

TEST_F(IpcTest, FindDeadlockCycleDetectsTwoNodeCycle) {
    RegisterWaitFor(/*waiter=*/1, /*blocker=*/2);
    RegisterWaitFor(/*waiter=*/2, /*blocker=*/1);
    std::vector<int> cycle;
    EXPECT_TRUE(FindDeadlockCycle(1, &cycle));
    EXPECT_FALSE(cycle.empty());
}

TEST_F(IpcTest, FindDeadlockCycleReturnsFalseForAcyclic) {
    RegisterWaitFor(1, 2);
    RegisterWaitFor(2, 3);
    std::vector<int> cycle;
    EXPECT_FALSE(FindDeadlockCycle(1, &cycle));
}

TEST_F(IpcTest, HasDeadlockDetectsLargerCycle) {
    // 1 -> 2 -> 3 -> 4 -> 1
    RegisterWaitFor(1, 2);
    RegisterWaitFor(2, 3);
    RegisterWaitFor(3, 4);
    RegisterWaitFor(4, 1);
    EXPECT_TRUE(HasDeadlock());
}

TEST_F(IpcTest, UnregisterWaitForRemovesEdge) {
    RegisterWaitFor(1, 2);
    RegisterWaitFor(2, 1);
    EXPECT_TRUE(HasDeadlock());
    UnregisterWaitFor(1);
    EXPECT_FALSE(HasDeadlock());
}

TEST_F(IpcTest, WaitForEdgesReturnsAllRegistered) {
    RegisterWaitFor(1, 2);
    RegisterWaitFor(3, 4);
    auto edges = WaitForEdges();
    EXPECT_EQ(edges.size(), 2u);
}

// =============================================================================
// fd tests
// =============================================================================

TEST_F(IpcTest, PathNameOpenFileCreatesNewFile) {
    File f = PathNameOpenFile(kTmpFile, kOCreate | kOReadWrite | kOExclusive);
    EXPECT_NE(f, kInvalidFile);
    EXPECT_EQ(NumOpenFiles(), 1);
    EXPECT_STREQ(FileName(f), kTmpFile);
    FileClose(f);
    EXPECT_EQ(NumOpenFiles(), 0);
}

TEST_F(IpcTest, FileWriteThenFileReadRoundtrips) {
    File f = PathNameOpenFile(kTmpFile, kOCreate | kOReadWrite | kOExclusive);
    ASSERT_NE(f, kInvalidFile);
    const char data[] = "hello world";
    int64_t off = 0;
    int written = FileWrite(f, data, sizeof(data), &off);
    EXPECT_EQ(written, static_cast<int>(sizeof(data)));
    EXPECT_EQ(off, static_cast<int64_t>(sizeof(data)));
    // Read back from start.
    char buf[32] = {0};
    int64_t read_off = 0;
    int read_n = FileRead(f, buf, sizeof(buf), &read_off);
    EXPECT_EQ(read_n, static_cast<int>(sizeof(data)));
    EXPECT_STREQ(buf, data);
    FileClose(f);
}

TEST_F(IpcTest, FileSeekToOffset) {
    File f = PathNameOpenFile(kTmpFile, kOCreate | kOReadWrite | kOExclusive);
    ASSERT_NE(f, kInvalidFile);
    const char data[] = "abcdefghij";
    int64_t off = 0;
    FileWrite(f, data, sizeof(data), &off);
    int64_t new_off = FileSeek(f, 3, SEEK_SET);
    EXPECT_EQ(new_off, 3);
    char buf[4] = {0};
    int64_t read_off = 3;
    FileRead(f, buf, 3, &read_off);
    EXPECT_EQ(std::string(buf, 3), "def");
    FileClose(f);
}

TEST_F(IpcTest, FileTruncateShortensFile) {
    File f = PathNameOpenFile(kTmpFile, kOCreate | kOReadWrite | kOExclusive);
    ASSERT_NE(f, kInvalidFile);
    const char data[] = "0123456789";
    int64_t off = 0;
    FileWrite(f, data, sizeof(data), &off);
    EXPECT_EQ(FileTruncate(f, 5), 0);
    int64_t new_off = FileSeek(f, 0, SEEK_END);
    EXPECT_EQ(new_off, 5);
    FileClose(f);
}

TEST_F(IpcTest, FileSyncReturnsZero) {
    File f = PathNameOpenFile(kTmpFile, kOCreate | kOReadWrite | kOExclusive);
    ASSERT_NE(f, kInvalidFile);
    EXPECT_EQ(FileSync(f), 0);
    FileClose(f);
}

TEST_F(IpcTest, AllocateFileAndFreeFile) {
    FILE* fp = static_cast<FILE*>(AllocateFile(kTmpFile, "w"));
    ASSERT_NE(fp, nullptr);
    EXPECT_EQ(NumOpenFiles(), 1);
    std::fputs("test", fp);
    EXPECT_EQ(FreeFile(fp), 0);
    EXPECT_EQ(NumOpenFiles(), 0);
}

TEST_F(IpcTest, CloseTransientFilesClosesAllocateFileHandles) {
    FILE* fp = static_cast<FILE*>(AllocateFile(kTmpFile, "w"));
    ASSERT_NE(fp, nullptr);
    CloseTransientFiles();
    EXPECT_EQ(NumOpenFiles(), 0);
    // fp was closed by CloseTransientFiles; nothing else to free.
}

// =============================================================================
// sync tests
// =============================================================================

TEST_F(IpcTest, RegisterSyncRequestEnqueuesRequest) {
    RelFileNode rnode{1, 100, 1000};
    SyncRequestTag tag{rnode, /*fork_num=*/0, /*segno=*/1};
    EXPECT_TRUE(RegisterSyncRequest(tag));
    EXPECT_EQ(NumPendingSyncRequests(), 1);
}

TEST_F(IpcTest, RegisterSyncRequestDeduplicates) {
    RelFileNode rnode{1, 100, 1000};
    SyncRequestTag tag{rnode, 0, 1};
    RegisterSyncRequest(tag);
    RegisterSyncRequest(tag);  // duplicate
    EXPECT_EQ(NumPendingSyncRequests(), 1);
}

TEST_F(IpcTest, ProcessSyncRequestsClearsQueue) {
    RelFileNode rnode{1, 100, 1000};
    RegisterSyncRequest(SyncRequestTag{rnode, 0, 1});
    RegisterSyncRequest(SyncRequestTag{rnode, 0, 2});
    int processed = pgcpp::storage::ProcessSyncRequests();
    EXPECT_EQ(processed, 2);
    EXPECT_EQ(NumPendingSyncRequests(), 0);
}

TEST_F(IpcTest, SyncFileReturnsZero) {
    RelFileNode rnode{1, 100, 1000};
    SyncRequestTag tag{rnode, 0, 1};
    EXPECT_EQ(SyncFile(tag), 0);
}

TEST_F(IpcTest, SyncStatsTrackCounts) {
    RelFileNode rnode{1, 100, 1000};
    RegisterSyncRequest(SyncRequestTag{rnode, 0, 1});
    RegisterSyncRequest(SyncRequestTag{rnode, 0, 2});
    pgcpp::storage::ProcessSyncRequests();
    SyncStats stats = GetSyncStats();
    EXPECT_EQ(stats.total_requests, 2);
    EXPECT_EQ(stats.total_processed, 2);
    EXPECT_EQ(stats.total_errors, 0);
}

TEST_F(IpcTest, GetPendingSyncRequestsReturnsSnapshot) {
    RelFileNode rnode{1, 100, 1000};
    RegisterSyncRequest(SyncRequestTag{rnode, 0, 1});
    RegisterSyncRequest(SyncRequestTag{rnode, 0, 2});
    auto reqs = GetPendingSyncRequests();
    EXPECT_EQ(reqs.size(), 2u);
}

// =============================================================================
// freespace tests
// =============================================================================

TEST_F(IpcTest, RecordPageWithFreeSpaceAndQuery) {
    RelFileNode rnode{1, 100, 1000};
    RecordPageWithFreeSpace(rnode, /*block=*/0, /*cat=*/10);
    RecordPageWithFreeSpace(rnode, /*block=*/1, /*cat=*/50);
    RecordPageWithFreeSpace(rnode, /*block=*/2, /*cat=*/20);
    // Find first block with cat >= 20.
    EXPECT_EQ(GetPageWithFreeSpace(rnode, /*min_cat=*/20), 1u);  // block 1 has 50
    // Find first block with cat >= 60 — none.
    EXPECT_EQ(GetPageWithFreeSpace(rnode, 60), kInvalidBlockNumber);
}

TEST_F(IpcTest, FreeSpaceMapTruncateRelDropsEntriesBeyondNblocks) {
    RelFileNode rnode{1, 100, 1000};
    RecordPageWithFreeSpace(rnode, 0, 10);
    RecordPageWithFreeSpace(rnode, 1, 20);
    RecordPageWithFreeSpace(rnode, 5, 30);
    FreeSpaceMapTruncateRel(rnode, /*nblocks=*/2);
    auto entries = GetFreeSpaceMapForRel(rnode);
    EXPECT_EQ(entries.size(), 2u);
}

TEST_F(IpcTest, FreeSpaceMapVacuumRelClearsRelation) {
    RelFileNode rnode{1, 100, 1000};
    RecordPageWithFreeSpace(rnode, 0, 10);
    RecordPageWithFreeSpace(rnode, 1, 20);
    int removed = FreeSpaceMapVacuumRel(rnode);
    EXPECT_EQ(removed, 2);
    EXPECT_TRUE(GetFreeSpaceMapForRel(rnode).empty());
}

TEST_F(IpcTest, FreeSpaceMapDropRelAliasForVacuum) {
    RelFileNode rnode{1, 100, 1000};
    RecordPageWithFreeSpace(rnode, 0, 10);
    EXPECT_EQ(FreeSpaceMapDropRel(rnode), 1);
    EXPECT_TRUE(GetFreeSpaceMapForRel(rnode).empty());
}

TEST_F(IpcTest, FreeSpaceCategoryForBytesRoundsDown) {
    EXPECT_EQ(FreeSpaceCategoryForBytes(0), 0);
    EXPECT_EQ(FreeSpaceCategoryForBytes(31), 0);  // < 32 → 0
    EXPECT_EQ(FreeSpaceCategoryForBytes(32), 1);  // 32 → 1
    EXPECT_EQ(FreeSpaceCategoryForBytes(63), 1);
    EXPECT_EQ(FreeSpaceCategoryForBytes(64), 2);
    // BytesForFreeSpaceCategory is the inverse lower-bound.
    EXPECT_EQ(BytesForFreeSpaceCategory(2), 64);
}

TEST_F(IpcTest, FreeSpaceCategoryForBytesClampsToMax) {
    EXPECT_EQ(FreeSpaceCategoryForBytes(100000), pgcpp::storage::kFsmFreeSpaceMax);
}

// =============================================================================
// fsmpage tests
// =============================================================================

TEST_F(IpcTest, FSMPageInitZeroesAllSlots) {
    FSMPageData page;
    FSMPageInit(&page);
    EXPECT_EQ(FSMPageGetFreeSpace(&page), 0);
}

TEST_F(IpcTest, FSMPageSetFreeSpaceUpdatesRoot) {
    FSMPageData page;
    FSMPageInit(&page);
    FSMPageSetFreeSpace(&page, /*slot=*/0, /*value=*/42);
    EXPECT_EQ(FSMPageGetFreeSpace(&page), 42);
}

TEST_F(IpcTest, FSMPageRootTracksMaximumOfLeaves) {
    FSMPageData page;
    FSMPageInit(&page);
    FSMPageSetFreeSpace(&page, 0, 10);
    FSMPageSetFreeSpace(&page, 5, 99);
    FSMPageSetFreeSpace(&page, 100, 50);
    EXPECT_EQ(FSMPageGetFreeSpace(&page), 99);
}

TEST_F(IpcTest, FSMPageSearchFreeSpaceFindsFirstFit) {
    FSMPageData page;
    FSMPageInit(&page);
    FSMPageSetFreeSpace(&page, 0, 10);
    FSMPageSetFreeSpace(&page, 5, 50);
    FSMPageSetFreeSpace(&page, 100, 20);
    int slot = FSMPageSearchFreeSpace(&page, /*min_value=*/30);
    EXPECT_EQ(slot, 5);  // only slot 5 has >= 30
}

TEST_F(IpcTest, FSMPageSearchFreeSpaceReturnsMinusOneIfNone) {
    FSMPageData page;
    FSMPageInit(&page);
    FSMPageSetFreeSpace(&page, 0, 10);
    FSMPageSetFreeSpace(&page, 5, 20);
    EXPECT_EQ(FSMPageSearchFreeSpace(&page, 99), -1);
}

TEST_F(IpcTest, FSMPageGetSlotReturnsSetValues) {
    FSMPageData page;
    FSMPageInit(&page);
    FSMPageSetFreeSpace(&page, 10, 77);
    EXPECT_EQ(FSMPageGetSlot(&page, 10), 77);
}

TEST_F(IpcTest, FSMPageClearResetsAllSlots) {
    FSMPageData page;
    FSMPageInit(&page);
    FSMPageSetFreeSpace(&page, 0, 99);
    FSMPageSetFreeSpace(&page, 5, 50);
    FSMPageClear(&page);
    EXPECT_EQ(FSMPageGetFreeSpace(&page), 0);
    EXPECT_EQ(FSMPageGetSlot(&page, 0), 0);
    EXPECT_EQ(FSMPageGetSlot(&page, 5), 0);
}

// =============================================================================
// large_object (inv_api) tests
// =============================================================================

TEST_F(IpcTest, InvCreateWithExplicitOid) {
    LargeObjectOid oid = inv_create(/*oid=*/5000);
    EXPECT_EQ(oid, 5000);
    EXPECT_NE(GetLargeObject(5000), nullptr);
    EXPECT_EQ(NumLargeObjects(), 1);
}

TEST_F(IpcTest, InvCreateAutoAssignsOid) {
    LargeObjectOid oid1 = inv_create(kInvalidLargeObjectOid);
    LargeObjectOid oid2 = inv_create(kInvalidLargeObjectOid);
    EXPECT_NE(oid1, oid2);
    EXPECT_GT(oid2, oid1);
}

TEST_F(IpcTest, InvOpenReturnsNullptrForMissing) {
    EXPECT_EQ(inv_open(99999, kInvRdwr), nullptr);
}

TEST_F(IpcTest, InvWriteAndInvReadRoundtrip) {
    LargeObjectOid oid = inv_create(5000);
    auto* desc = inv_open(oid, kInvWrite);
    ASSERT_NE(desc, nullptr);
    const uint8_t data[] = {'a', 'b', 'c', 'd', 'e'};
    int written = inv_write(desc, data, sizeof(data));
    EXPECT_EQ(written, static_cast<int>(sizeof(data)));
    // Seek back to start and read.
    EXPECT_EQ(inv_seek(desc, 0, SEEK_SET), 0);
    uint8_t buf[16] = {0};
    int read_n = inv_read(desc, buf, sizeof(buf));
    EXPECT_EQ(read_n, static_cast<int>(sizeof(data)));
    EXPECT_EQ(std::memcmp(buf, data, sizeof(data)), 0);
    inv_close(desc);
}

TEST_F(IpcTest, InvSeekCurAndEnd) {
    LargeObjectOid oid = inv_create(5000);
    auto* desc = inv_open(oid, kInvRdwr);
    ASSERT_NE(desc, nullptr);
    const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    inv_write(desc, data, sizeof(data));
    EXPECT_EQ(inv_tell(desc), 8);
    EXPECT_EQ(inv_seek(desc, -2, SEEK_END), 6);  // 8 - 2 = 6
    EXPECT_EQ(inv_tell(desc), 6);
    EXPECT_EQ(inv_seek(desc, 2, SEEK_CUR), 8);  // 6 + 2 = 8
    inv_close(desc);
}

TEST_F(IpcTest, InvTruncateShortensAndAdjustsOffset) {
    LargeObjectOid oid = inv_create(5000);
    auto* desc = inv_open(oid, kInvRdwr);
    ASSERT_NE(desc, nullptr);
    const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    inv_write(desc, data, sizeof(data));
    EXPECT_EQ(inv_truncate(desc, /*length=*/3), 0);
    auto* lo = GetLargeObject(oid);
    ASSERT_NE(lo, nullptr);
    EXPECT_EQ(lo->data.size(), 3u);
    EXPECT_EQ(inv_tell(desc), 3);  // offset was beyond new length, clamped
    inv_close(desc);
}

TEST_F(IpcTest, InvReadAtEofReturnsZero) {
    LargeObjectOid oid = inv_create(5000);
    auto* desc = inv_open(oid, kInvRdwr);
    ASSERT_NE(desc, nullptr);
    const uint8_t data[] = {1, 2, 3};
    inv_write(desc, data, sizeof(data));
    inv_seek(desc, 0, SEEK_SET);
    uint8_t buf[16] = {0};
    EXPECT_EQ(inv_read(desc, buf, 3), 3);
    EXPECT_EQ(inv_read(desc, buf, 3), 0);  // EOF
    inv_close(desc);
}

TEST_F(IpcTest, InvDropRemovesLargeObject) {
    LargeObjectOid oid = inv_create(5000);
    EXPECT_NE(GetLargeObject(oid), nullptr);
    EXPECT_EQ(inv_drop(oid), 0);
    EXPECT_EQ(GetLargeObject(oid), nullptr);
    EXPECT_EQ(NumLargeObjects(), 0);
}

TEST_F(IpcTest, InvDropReturnsErrorForMissing) {
    EXPECT_EQ(inv_drop(99999), -1);
}
