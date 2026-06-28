// wal_multiversion_test.cpp — Unit tests for Task 15.16 (M7 WAL + multiversion).
//
// Tests:
//   - WAL record write/replay (XLogInsert + XLogReader + crash recovery)
//   - Crash recovery (PerformCrashRecovery dispatches to redo functions)
//   - SLRU page cache (write/read/flush/eviction)
//   - Commit timestamps (set/get/transaction tree)
//   - MultiXact (create/expand/get members)
//   - ProcArray (add/remove/oldest Xmin/running XIDs)
//   - Shared cache invalidation (enqueue/dispatch/handlers)

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/transaction/commit_ts.hpp"
#include "pgcpp/transaction/multixact.hpp"
#include "pgcpp/transaction/parallel.hpp"
#include "pgcpp/transaction/procarray.hpp"
#include "pgcpp/transaction/sinval.hpp"
#include "pgcpp/transaction/slru.hpp"
#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xact.hpp"
#include "pgcpp/transaction/xlog.hpp"
#include "pgcpp/transaction/xloginsert.hpp"
#include "pgcpp/transaction/xlogreader.hpp"
#include "pgcpp/transaction/xlogrecovery.hpp"
#include "pgcpp/transaction/xlogutils.hpp"

using mytoydb::transaction::AcceptInvalidationMessages;
using mytoydb::transaction::CacheInvalidateCatcache;
using mytoydb::transaction::CacheInvalidateRelcache;
using mytoydb::transaction::CacheInvalidateSmgr;
using mytoydb::transaction::CacheInvalidateSnapshot;
using mytoydb::transaction::CommitTsEntry;
using mytoydb::transaction::CountRunningXacts;
using mytoydb::transaction::CreateParallelContext;
using mytoydb::transaction::DestroyParallelContext;
using mytoydb::transaction::GetOldestXmin;
using mytoydb::transaction::GetPendingInvalidationCount;
using mytoydb::transaction::GetRegisteredData;
using mytoydb::transaction::GetRunningTransactionData;
using mytoydb::transaction::GetWalBuffer;
using mytoydb::transaction::GetWalBufferSize;
using mytoydb::transaction::GetXLogInsertRecPtr;
using mytoydb::transaction::GetXLogWriteRecPtr;
using mytoydb::transaction::InitializeCommitTs;
using mytoydb::transaction::InitializeMultiXact;
using mytoydb::transaction::InitializeParallelInfrastructure;
using mytoydb::transaction::InitializeProcArray;
using mytoydb::transaction::InitializeSinval;
using mytoydb::transaction::InitializeTransactionSystem;
using mytoydb::transaction::InitializeWal;
using mytoydb::transaction::InvalidHandler;
using mytoydb::transaction::kFirstMultiXactId;
using mytoydb::transaction::kInvalidMultiXactId;
using mytoydb::transaction::kInvalidTransactionId;
using mytoydb::transaction::kInvalidXLogRecPtr;
using mytoydb::transaction::kRmgrCommitTsId;
using mytoydb::transaction::kRmgrHeapId;
using mytoydb::transaction::kRmgrXactId;
using mytoydb::transaction::kSizeofXlogRecord;
using mytoydb::transaction::LaunchParallelWorkers;
using mytoydb::transaction::MultiXactId;
using mytoydb::transaction::MultiXactIdCreate;
using mytoydb::transaction::MultiXactIdExpand;
using mytoydb::transaction::MultiXactIdGetMembers;
using mytoydb::transaction::MultiXactIdIsValid;
using mytoydb::transaction::MultiXactMember;
using mytoydb::transaction::ParallelContext;
using mytoydb::transaction::ParallelContextActive;
using mytoydb::transaction::PerformCrashRecovery;
using mytoydb::transaction::PerformCrashRecoveryFrom;
using mytoydb::transaction::ProcArrayAdd;
using mytoydb::transaction::ProcArrayContains;
using mytoydb::transaction::ProcArrayRemove;
using mytoydb::transaction::RecoveryStats;
using mytoydb::transaction::RedoFn;
using mytoydb::transaction::RegisterInvalidationHandler;
using mytoydb::transaction::RegisterRmgrRedo;
using mytoydb::transaction::ResetCommitTs;
using mytoydb::transaction::ResetMultiXact;
using mytoydb::transaction::ResetProcArray;
using mytoydb::transaction::ResetSinval;
using mytoydb::transaction::ResetTransactionState;
using mytoydb::transaction::ResetWal;
using mytoydb::transaction::ResetXlogInsertState;
using mytoydb::transaction::RmgrId;
using mytoydb::transaction::SendSharedInvalidationMessages;
using mytoydb::transaction::SharedInvalCmdType;
using mytoydb::transaction::SharedInvalidationMessage;
using mytoydb::transaction::SimpleLruFlush;
using mytoydb::transaction::SimpleLruFree;
using mytoydb::transaction::SimpleLruInit;
using mytoydb::transaction::SimpleLruRead;
using mytoydb::transaction::SimpleLruReset;
using mytoydb::transaction::SimpleLruWrite;
using mytoydb::transaction::SlruCtl;
using mytoydb::transaction::SlruPageStatus;
using mytoydb::transaction::TimestampTz;
using mytoydb::transaction::TransactionId;
using mytoydb::transaction::TransactionIdGetCommitTs;
using mytoydb::transaction::TransactionIdSetCommitTs;
using mytoydb::transaction::TransactionTreeSetCommitTsData;
using mytoydb::transaction::UnregisterInvalidationHandler;
using mytoydb::transaction::XLByteEQ;
using mytoydb::transaction::XLByteGE;
using mytoydb::transaction::XLByteGT;
using mytoydb::transaction::XLByteLE;
using mytoydb::transaction::XLByteLT;
using mytoydb::transaction::XLogBeginInsert;
using mytoydb::transaction::XLogFlush;
using mytoydb::transaction::XLogInsert;
using mytoydb::transaction::XLogReaderAlloc;
using mytoydb::transaction::XLogReaderFree;
using mytoydb::transaction::XLogReaderState;
using mytoydb::transaction::XLogReadRaw;
using mytoydb::transaction::XLogReadRecord;
using mytoydb::transaction::XLogReadRecordAt;
using mytoydb::transaction::XLogRecord;
using mytoydb::transaction::XLogRecPtr;
using mytoydb::transaction::XLogRegisterData;
using mytoydb::transaction::XLogResetInsert;
using mytoydb::transaction::XLogSetRecordFlags;
using mytoydb::transaction::XLogWriteRaw;

namespace {

class WalMultiversionTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = mytoydb::memory::AllocSetContext::Create("wal_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeWal();
        InitializeCommitTs();
        InitializeMultiXact();
        InitializeProcArray();
        InitializeSinval();
    }

    void TearDown() override {
        ResetTransactionState();
        ResetWal();
        ResetXlogInsertState();
        ResetCommitTs();
        ResetMultiXact();
        ResetProcArray();
        ResetSinval();

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    mytoydb::memory::MemoryContext* context_ = nullptr;
};

// ===========================================================================
// WAL buffer tests
// ===========================================================================

// InitializeWal clears the buffer and resets the insert pointer.
TEST_F(WalMultiversionTest, InitializeWal_ClearsBuffer) {
    // Write some data first.
    uint32_t data = 42;
    XLogWriteRaw(&data, sizeof(data));
    EXPECT_GT(GetWalBufferSize(), 0u);

    // Re-initialize.
    InitializeWal();
    EXPECT_EQ(GetWalBufferSize(), static_cast<std::size_t>(kSizeofXlogRecord));
    EXPECT_EQ(GetXLogInsertRecPtr(), static_cast<XLogRecPtr>(kSizeofXlogRecord));
}

// XLogWriteRaw appends bytes and returns the starting LSN.
TEST_F(WalMultiversionTest, XLogWriteRaw_AppendsAndReturnsLsn) {
    uint32_t data = 0x12345678;
    XLogRecPtr lsn = XLogWriteRaw(&data, sizeof(data));
    EXPECT_EQ(lsn, static_cast<XLogRecPtr>(kSizeofXlogRecord));

    // The data should be readable back.
    uint32_t read_back = 0;
    std::size_t n = XLogReadRaw(lsn, &read_back, sizeof(read_back));
    EXPECT_EQ(n, sizeof(read_back));
    EXPECT_EQ(read_back, data);
}

// XLogReadRaw returns 0 for LSN past the end of WAL.
TEST_F(WalMultiversionTest, XLogReadRaw_ReturnsZeroPastEnd) {
    uint8_t buf[4];
    std::size_t n = XLogReadRaw(999999, buf, sizeof(buf));
    EXPECT_EQ(n, 0u);
}

// XLogFlush is a no-op in MyToyDB (in-memory buffer is always durable).
TEST_F(WalMultiversionTest, XLogFlush_NoOp) {
    XLogRecPtr before = GetXLogWriteRecPtr();
    XLogFlush(GetXLogInsertRecPtr());
    EXPECT_EQ(GetXLogWriteRecPtr(), before);
}

// LSN comparison helpers.
TEST_F(WalMultiversionTest, LsnComparison) {
    EXPECT_TRUE(XLByteLT(1, 2));
    EXPECT_TRUE(XLByteLE(1, 2));
    EXPECT_TRUE(XLByteEQ(2, 2));
    EXPECT_TRUE(XLByteGT(3, 2));
    EXPECT_TRUE(XLByteGE(3, 3));
    EXPECT_FALSE(XLByteGT(2, 3));
}

// ===========================================================================
// WAL record insertion (XLogInsert)
// ===========================================================================

// XLogInsert writes a record with the correct header fields.
TEST_F(WalMultiversionTest, XLogInsert_WritesCorrectHeader) {
    uint32_t payload = 0xDEADBEEF;
    XLogBeginInsert();
    XLogRegisterData(&payload, sizeof(payload));
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0x10);

    EXPECT_NE(lsn, kInvalidXLogRecPtr);

    // Read the record back.
    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    EXPECT_EQ(reader->record.xl_rmid, kRmgrHeapId);
    EXPECT_EQ(reader->record.xl_info, 0x10);
    EXPECT_EQ(reader->record.xl_tot_len,
              static_cast<uint32_t>(kSizeofXlogRecord + sizeof(payload)));
    ASSERT_EQ(reader->main_data.size(), sizeof(payload));
    uint32_t read_payload = 0;
    std::memcpy(&read_payload, reader->main_data.data(), sizeof(read_payload));
    EXPECT_EQ(read_payload, payload);
    XLogReaderFree(reader);
}

// Multiple XLogInsert calls chain via xl_prev.
TEST_F(WalMultiversionTest, XLogInsert_ChainsRecordsViaPrev) {
    XLogBeginInsert();
    XLogResetInsert();  // clear any partial state

    uint32_t data1 = 1;
    XLogBeginInsert();
    XLogRegisterData(&data1, sizeof(data1));
    XLogRecPtr lsn1 = XLogInsert(kRmgrHeapId, 0x01);

    uint32_t data2 = 2;
    XLogBeginInsert();
    XLogRegisterData(&data2, sizeof(data2));
    XLogRecPtr lsn2 = XLogInsert(kRmgrHeapId, 0x02);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn2));
    EXPECT_EQ(reader->record.xl_prev, lsn1);

    ASSERT_TRUE(XLogReadRecordAt(reader, lsn1));
    EXPECT_EQ(reader->record.xl_prev, static_cast<XLogRecPtr>(0));
    XLogReaderFree(reader);
}

// XLogRegisterData can be called multiple times to accumulate payload.
TEST_F(WalMultiversionTest, XLogRegisterData_AccumulatesMultipleChunks) {
    uint32_t a = 0xAA;
    uint32_t b = 0xBB;
    XLogBeginInsert();
    XLogRegisterData(&a, sizeof(a));
    XLogRegisterData(&b, sizeof(b));
    XLogRecPtr lsn = XLogInsert(kRmgrXactId, 0);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    ASSERT_EQ(reader->main_data.size(), sizeof(a) + sizeof(b));
    EXPECT_EQ(0, std::memcmp(reader->main_data.data(), &a, sizeof(a)));
    EXPECT_EQ(0, std::memcmp(reader->main_data.data() + sizeof(a), &b, sizeof(b)));
    XLogReaderFree(reader);
}

// ===========================================================================
// XLogReader iteration
// ===========================================================================

// XLogReadRecord iterates through all records in order.
TEST_F(WalMultiversionTest, XLogReadRecord_IteratesAllRecords) {
    uint32_t vals[] = {10, 20, 30};
    XLogRecPtr lsns[3];
    for (int i = 0; i < 3; i++) {
        XLogBeginInsert();
        XLogRegisterData(&vals[i], sizeof(vals[i]));
        lsns[i] = XLogInsert(kRmgrHeapId, static_cast<uint8_t>(i));
    }

    auto* reader = XLogReaderAlloc();
    XLogRecPtr lsn = lsns[0];  // start from first record
    int count = 0;
    while (XLogReadRecord(reader, &lsn)) {
        ASSERT_EQ(reader->main_data.size(), sizeof(uint32_t));
        uint32_t val = 0;
        std::memcpy(&val, reader->main_data.data(), sizeof(val));
        EXPECT_EQ(val, vals[count]);
        count++;
    }
    EXPECT_EQ(count, 3);
    XLogReaderFree(reader);
}

// XLogReadRecord returns false at end-of-WAL.
TEST_F(WalMultiversionTest, XLogReadRecord_ReturnsFalseAtEndOfWal) {
    uint32_t val = 42;
    XLogBeginInsert();
    XLogRegisterData(&val, sizeof(val));
    XLogInsert(kRmgrHeapId, 0);

    auto* reader = XLogReaderAlloc();
    XLogRecPtr lsn = kSizeofXlogRecord;
    ASSERT_TRUE(XLogReadRecord(reader, &lsn));
    EXPECT_FALSE(XLogReadRecord(reader, &lsn));  // no more records
    EXPECT_TRUE(reader->end_of_wal);
    XLogReaderFree(reader);
}

// ===========================================================================
// Crash recovery
// ===========================================================================

// PerformCrashRecovery replays all records via registered redo functions.
TEST_F(WalMultiversionTest, PerformCrashRecovery_ReplaysRecords) {
    // Track replayed records in a vector.
    std::vector<std::pair<RmgrId, XLogRecPtr>> replayed;

    RegisterRmgrRedo(kRmgrHeapId, [&](const XLogRecord& rec, const uint8_t* data, std::size_t len,
                                      XLogRecPtr lsn) {
        replayed.push_back({rec.xl_rmid, lsn});
        EXPECT_EQ(len, sizeof(uint32_t));
        uint32_t val = 0;
        std::memcpy(&val, data, sizeof(val));
        EXPECT_EQ(val, 100);
    });

    uint32_t val = 100;
    XLogBeginInsert();
    XLogRegisterData(&val, sizeof(val));
    XLogInsert(kRmgrHeapId, 0x10);

    RecoveryStats stats = PerformCrashRecovery();
    EXPECT_EQ(stats.records_replayed, 1u);
    EXPECT_EQ(stats.records_skipped, 0u);
    ASSERT_EQ(replayed.size(), 1u);
    EXPECT_EQ(replayed[0].first, kRmgrHeapId);
}

// PerformCrashRecovery counts skipped records for unregistered RMGRs.
TEST_F(WalMultiversionTest, PerformCrashRecovery_SkipsUnregisteredRmgr) {
    uint32_t val = 1;
    XLogBeginInsert();
    XLogRegisterData(&val, sizeof(val));
    XLogInsert(kRmgrCommitTsId, 0);  // no redo registered

    RecoveryStats stats = PerformCrashRecovery();
    EXPECT_EQ(stats.records_replayed, 0u);
    EXPECT_EQ(stats.records_skipped, 1u);
}

// PerformCrashRecovery handles multiple records with mixed RMGRs.
TEST_F(WalMultiversionTest, PerformCrashRecovery_HandlesMixedRmgrs) {
    int heap_count = 0;
    int xact_count = 0;
    RegisterRmgrRedo(kRmgrHeapId, [&](const XLogRecord&, const uint8_t*, std::size_t, XLogRecPtr) {
        heap_count++;
    });
    RegisterRmgrRedo(kRmgrXactId, [&](const XLogRecord&, const uint8_t*, std::size_t, XLogRecPtr) {
        xact_count++;
    });

    for (int i = 0; i < 3; i++) {
        uint32_t val = i;
        XLogBeginInsert();
        XLogRegisterData(&val, sizeof(val));
        XLogInsert(kRmgrHeapId, 0);
    }
    for (int i = 0; i < 2; i++) {
        uint32_t val = i;
        XLogBeginInsert();
        XLogRegisterData(&val, sizeof(val));
        XLogInsert(kRmgrXactId, 0);
    }

    RecoveryStats stats = PerformCrashRecovery();
    EXPECT_EQ(stats.records_replayed, 5u);
    EXPECT_EQ(heap_count, 3);
    EXPECT_EQ(xact_count, 2);
}

// Crash recovery simulates a full crash-and-recover cycle.
TEST_F(WalMultiversionTest, CrashRecovery_FullCycle) {
    // Simulate: write 5 WAL records, "crash" (no state reset), recover.
    std::vector<uint32_t> written;
    for (int i = 0; i < 5; i++) {
        uint32_t val = static_cast<uint32_t>(i * 1000);
        XLogBeginInsert();
        XLogRegisterData(&val, sizeof(val));
        XLogInsert(kRmgrHeapId, 0);
        written.push_back(val);
    }

    std::vector<uint32_t> replayed_vals;
    RegisterRmgrRedo(kRmgrHeapId,
                     [&](const XLogRecord&, const uint8_t* data, std::size_t len, XLogRecPtr) {
                         ASSERT_EQ(len, sizeof(uint32_t));
                         uint32_t v = 0;
                         std::memcpy(&v, data, sizeof(v));
                         replayed_vals.push_back(v);
                     });

    RecoveryStats stats = PerformCrashRecovery();
    EXPECT_EQ(stats.records_replayed, 5u);
    ASSERT_EQ(replayed_vals.size(), written.size());
    for (std::size_t i = 0; i < written.size(); i++) {
        EXPECT_EQ(replayed_vals[i], written[i]);
    }
}

// ===========================================================================
// SLRU tests
// ===========================================================================

// SimpleLruWrite/Read round-trip data.
TEST_F(WalMultiversionTest, SlruWriteRead_RoundTrip) {
    auto* ctl = SimpleLruInit("test_slru", 4);
    uint32_t val = 0xCAFEBABE;
    SimpleLruWrite(ctl, 0, 100, &val, sizeof(val));

    uint32_t read_back = 0;
    SimpleLruRead(ctl, 0, 100, &read_back, sizeof(read_back));
    EXPECT_EQ(read_back, val);
    SimpleLruFree(ctl);
}

// SimpleLruFlush marks dirty pages as valid.
TEST_F(WalMultiversionTest, SlruFlush_ClearsDirtyFlags) {
    auto* ctl = SimpleLruInit("test_slru", 4);
    uint32_t val = 42;
    SimpleLruWrite(ctl, 0, 0, &val, sizeof(val));
    EXPECT_GT(ctl->writes, 0u);

    SimpleLruFlush(ctl);
    EXPECT_GT(ctl->flushes, 0u);

    // Data should still be readable after flush.
    uint32_t read_back = 0;
    SimpleLruRead(ctl, 0, 0, &read_back, sizeof(read_back));
    EXPECT_EQ(read_back, val);
    SimpleLruFree(ctl);
}

// SLRU evicts pages when the cache is full (FIFO eviction of page 0).
TEST_F(WalMultiversionTest, SlruEvictsWhenFull) {
    auto* ctl = SimpleLruInit("test_slru", 2);
    // Fill cache with 2 pages.
    uint32_t v1 = 1, v2 = 2, v3 = 3;
    SimpleLruWrite(ctl, 0, 0, &v1, sizeof(v1));
    SimpleLruWrite(ctl, 1, 0, &v2, sizeof(v2));
    // Accessing a third page should evict page 0.
    SimpleLruWrite(ctl, 2, 0, &v3, sizeof(v3));
    // The cache should still have 2 pages.
    EXPECT_LE(ctl->pages.size(), ctl->capacity);
    SimpleLruFree(ctl);
}

// SimpleLruReset clears all pages and stats.
TEST_F(WalMultiversionTest, SlruReset_ClearsAll) {
    auto* ctl = SimpleLruInit("test_slru", 4);
    uint32_t val = 99;
    SimpleLruWrite(ctl, 0, 0, &val, sizeof(val));
    EXPECT_GT(ctl->writes, 0u);

    SimpleLruReset(ctl);
    EXPECT_EQ(ctl->pages.size(), 0u);
    EXPECT_EQ(ctl->reads, 0u);
    EXPECT_EQ(ctl->writes, 0u);
    SimpleLruFree(ctl);
}

// ===========================================================================
// Commit timestamp tests
// ===========================================================================

// TransactionIdSetCommitTs records and GetCommitTs retrieves.
TEST_F(WalMultiversionTest, CommitTs_SetGet) {
    TransactionId xid = 100;
    TimestampTz ts = 1234567890;
    TransactionIdSetCommitTs(xid, ts);
    EXPECT_EQ(TransactionIdGetCommitTs(xid), ts);
}

// TransactionIdGetCommitTs returns 0 for unknown XIDs.
TEST_F(WalMultiversionTest, CommitTs_GetReturnsZeroForUnknown) {
    EXPECT_EQ(TransactionIdGetCommitTs(99999), static_cast<TimestampTz>(0));
}

// TransactionTreeSetCommitTsData sets timestamps for the whole tree.
TEST_F(WalMultiversionTest, CommitTs_TreeSet) {
    TransactionId top = 200;
    TransactionId subs[] = {201, 202, 203};
    TimestampTz ts = 9999;
    TransactionTreeSetCommitTsData(top, ts, 3, subs);

    EXPECT_EQ(TransactionIdGetCommitTs(top), ts);
    EXPECT_EQ(TransactionIdGetCommitTs(201), ts);
    EXPECT_EQ(TransactionIdGetCommitTs(202), ts);
    EXPECT_EQ(TransactionIdGetCommitTs(203), ts);
}

// ===========================================================================
// MultiXact tests
// ===========================================================================

// MultiXactIdCreate returns sequential IDs.
TEST_F(WalMultiversionTest, MultiXact_CreateSequentialIds) {
    std::vector<MultiXactMember> members = {{10, 1}, {11, 2}};
    MultiXactId m1 = MultiXactIdCreate(members);
    MultiXactId m2 = MultiXactIdCreate(members);
    EXPECT_EQ(m1, kFirstMultiXactId);
    EXPECT_EQ(m2, kFirstMultiXactId + 1);
    EXPECT_TRUE(MultiXactIdIsValid(m1));
    EXPECT_FALSE(MultiXactIdIsValid(kInvalidMultiXactId));
}

// MultiXactIdGetMembers retrieves the members of a multixact.
TEST_F(WalMultiversionTest, MultiXact_GetMembers) {
    std::vector<MultiXactMember> members = {{5, 1}, {6, 2}, {7, 3}};
    MultiXactId multi = MultiXactIdCreate(members);

    auto retrieved = MultiXactIdGetMembers(multi);
    ASSERT_EQ(retrieved.size(), members.size());
    for (std::size_t i = 0; i < members.size(); i++) {
        EXPECT_EQ(retrieved[i].xid, members[i].xid);
        EXPECT_EQ(retrieved[i].status, members[i].status);
    }
}

// MultiXactIdExpand adds a member to an existing multixact.
TEST_F(WalMultiversionTest, MultiXact_ExpandAddsMember) {
    std::vector<MultiXactMember> members = {{10, 1}};
    MultiXactId multi = MultiXactIdCreate(members);
    EXPECT_EQ(MultiXactIdGetMembers(multi).size(), 1u);

    MultiXactId expanded = MultiXactIdExpand(multi, 20, 2);
    EXPECT_EQ(MultiXactIdGetMembers(expanded).size(), 2u);
    auto retrieved = MultiXactIdGetMembers(expanded);
    EXPECT_EQ(retrieved[1].xid, static_cast<TransactionId>(20));
}

// MultiXactIdExpand returns the same ID if the XID is already a member.
TEST_F(WalMultiversionTest, MultiXact_ExpandNoOpForExistingMember) {
    std::vector<MultiXactMember> members = {{10, 1}};
    MultiXactId multi = MultiXactIdCreate(members);
    MultiXactId result = MultiXactIdExpand(multi, 10, 1);
    EXPECT_EQ(result, multi);
    EXPECT_EQ(MultiXactIdGetMembers(multi).size(), 1u);
}

// MultiXactIdGetMembers returns empty for invalid ID.
TEST_F(WalMultiversionTest, MultiXact_GetMembersEmptyForInvalid) {
    auto retrieved = MultiXactIdGetMembers(kInvalidMultiXactId);
    EXPECT_TRUE(retrieved.empty());
}

// ===========================================================================
// ProcArray tests
// ===========================================================================

// ProcArrayAdd/Remove track running transactions.
TEST_F(WalMultiversionTest, ProcArray_AddRemove) {
    EXPECT_EQ(CountRunningXacts(), 0);
    ProcArrayAdd(10);
    ProcArrayAdd(20);
    EXPECT_EQ(CountRunningXacts(), 2);
    EXPECT_TRUE(ProcArrayContains(10));
    EXPECT_FALSE(ProcArrayContains(99));

    ProcArrayRemove(10);
    EXPECT_EQ(CountRunningXacts(), 1);
    EXPECT_FALSE(ProcArrayContains(10));
}

// GetOldestXmin returns the minimum running XID.
TEST_F(WalMultiversionTest, ProcArray_GetOldestXmin) {
    ProcArrayAdd(50);
    ProcArrayAdd(30);
    ProcArrayAdd(70);
    EXPECT_EQ(GetOldestXmin(), static_cast<TransactionId>(30));
}

// GetOldestXmin returns FrozenTransactionId when no transactions are running.
TEST_F(WalMultiversionTest, ProcArray_GetOldestXminEmpty) {
    EXPECT_EQ(GetOldestXmin(), mytoydb::transaction::kFrozenTransactionId);
}

// GetOldestXmin ignores the specified XID.
TEST_F(WalMultiversionTest, ProcArray_GetOldestXminIgnore) {
    ProcArrayAdd(30);
    ProcArrayAdd(50);
    EXPECT_EQ(GetOldestXmin(30), static_cast<TransactionId>(50));
}

// GetRunningTransactionData returns a copy of the running XIDs.
TEST_F(WalMultiversionTest, ProcArray_GetRunningData) {
    ProcArrayAdd(10);
    ProcArrayAdd(20);
    auto running = GetRunningTransactionData();
    EXPECT_EQ(running.size(), 2u);
}

// ===========================================================================
// Shared cache invalidation (sinval) tests
// ===========================================================================

// CacheInvalidateRelcache enqueues a message.
TEST_F(WalMultiversionTest, Sinval_EnqueueRelcache) {
    EXPECT_EQ(GetPendingInvalidationCount(), 0u);
    CacheInvalidateRelcache(100, 200);
    EXPECT_EQ(GetPendingInvalidationCount(), 1u);
}

// SendSharedInvalidationMessages dispatches to handlers and clears the queue.
TEST_F(WalMultiversionTest, Sinval_DispatchAndClear) {
    std::vector<SharedInvalidationMessage> received;
    int handler_id = RegisterInvalidationHandler(
        [&](const SharedInvalidationMessage& msg) { received.push_back(msg); });

    CacheInvalidateRelcache(1, 10);
    CacheInvalidateSnapshot(1);
    CacheInvalidateSmgr(1, 20);
    CacheInvalidateCatcache(1, 5, 99);
    EXPECT_EQ(GetPendingInvalidationCount(), 4u);

    SendSharedInvalidationMessages();
    EXPECT_EQ(GetPendingInvalidationCount(), 0u);
    ASSERT_EQ(received.size(), 4u);
    EXPECT_EQ(received[0].kind, SharedInvalCmdType::kRelcache);
    EXPECT_EQ(received[1].kind, SharedInvalCmdType::kSnapshot);
    EXPECT_EQ(received[2].kind, SharedInvalCmdType::kSmgr);
    EXPECT_EQ(received[3].kind, SharedInvalCmdType::kCatcache);

    UnregisterInvalidationHandler(handler_id);
}

// AcceptInvalidationMessages is an alias for SendSharedInvalidationMessages.
TEST_F(WalMultiversionTest, Sinval_AcceptIsAlias) {
    int count = 0;
    RegisterInvalidationHandler([&](const SharedInvalidationMessage&) { count++; });

    CacheInvalidateRelcache(1, 1);
    AcceptInvalidationMessages();
    EXPECT_EQ(count, 1);
    EXPECT_EQ(GetPendingInvalidationCount(), 0u);
}

// UnregisterInvalidationHandler stops a handler from receiving messages.
TEST_F(WalMultiversionTest, Sinval_UnregisterStopsHandler) {
    int count = 0;
    int id = RegisterInvalidationHandler([&](const SharedInvalidationMessage&) { count++; });

    CacheInvalidateRelcache(1, 1);
    SendSharedInvalidationMessages();
    EXPECT_EQ(count, 1);

    UnregisterInvalidationHandler(id);
    CacheInvalidateRelcache(1, 2);
    SendSharedInvalidationMessages();
    EXPECT_EQ(count, 1);  // still 1, not 2
}

// ===========================================================================
// Parallel stub tests
// ===========================================================================

// ParallelContextActive is always false in MyToyDB.
TEST_F(WalMultiversionTest, Parallel_AlwaysInactive) {
    EXPECT_FALSE(ParallelContextActive());
}

// CreateParallelContext/DestroyParallelContext round-trip.
TEST_F(WalMultiversionTest, Parallel_CreateDestroy) {
    InitializeParallelInfrastructure();
    auto* ctx = CreateParallelContext(4);
    EXPECT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->nworkers, 4);
    EXPECT_FALSE(ctx->initialized);

    int launched = LaunchParallelWorkers(ctx);
    EXPECT_EQ(launched, 0);  // no workers in MyToyDB
    EXPECT_TRUE(ctx->initialized);

    DestroyParallelContext(ctx);
}

}  // namespace
