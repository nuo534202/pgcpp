// aux_process_test.cpp — Unit tests for M12 auxiliary process modules.
//
// Covers Task 15.17 verification:
//   - 后台脏页刷盘 (background dirty page flushing) — bgwriter
//   - 检查点 (checkpoints) — checkpointer
//   - 自动 VACUUM (autovacuum) — autovacuum launcher/worker
//   - WAL 写入 (WAL writing) — walwriter
//   - 归档 (archiving) — pgarch + shell_archive
//
// Also covers: auxprocess dispatch, interrupt handling, fork_process,
// startup crash recovery, syslogger, bgworker registry.

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "server/autovacuum.hpp"
#include "server/auxprocess.hpp"
#include "server/bgworker.hpp"
#include "server/bgwriter.hpp"
#include "server/checkpointer.hpp"
#include "server/fork_process.hpp"
#include "server/interrupt.hpp"
#include "server/pgarch.hpp"
#include "server/shell_archive.hpp"
#include "server/startup.hpp"
#include "server/syslogger.hpp"
#include "server/walwriter.hpp"
#include "storage/bufmgr.hpp"
#include "storage/bufpage.hpp"
#include "storage/smgr.hpp"
#include "transaction/xlog.hpp"
#include "transaction/xloginsert.hpp"
#include "transaction/xlogrecovery.hpp"

using pgcpp::server::AutoVacuumStats;
using pgcpp::server::AutoVacuumWorkItem;
using pgcpp::server::AuxiliaryProcessType;
using pgcpp::server::AuxProcessMain;
using pgcpp::server::AuxProcessTypeToString;
using pgcpp::server::BackgroundWorker;
using pgcpp::server::BgWorkerState;
using pgcpp::server::BgWorkerType;
using pgcpp::server::BgWriterStats;
using pgcpp::server::CheckpointerIsRunning;
using pgcpp::server::CheckpointerMain;
using pgcpp::server::CheckpointStats;
using pgcpp::server::CloseStdio;
using pgcpp::server::CreateCheckPoint;
using pgcpp::server::GetBgWorkerState;
using pgcpp::server::GetBgWriterStats;
using pgcpp::server::GetCheckpointStats;
using pgcpp::server::GetPendingArchiveRequests;
using pgcpp::server::GetPgArchState;
using pgcpp::server::GetPgArchStats;
using pgcpp::server::GetShellArchiveStats;
using pgcpp::server::GetStartupState;
using pgcpp::server::GetStartupStats;
using pgcpp::server::GetSysLoggerMessages;
using pgcpp::server::GetSysLoggerState;
using pgcpp::server::GetSysLoggerStats;
using pgcpp::server::GetWalWriterStats;
using pgcpp::server::HandleInterrupts;
using pgcpp::server::InitializeAutoVacuum;
using pgcpp::server::InitializeBgWorker;
using pgcpp::server::InitializeBgWriter;
using pgcpp::server::InitializeCheckpointer;
using pgcpp::server::InitializePgArch;
using pgcpp::server::InitializeShellArchive;
using pgcpp::server::InitializeStartupProcess;
using pgcpp::server::InitializeSysLogger;
using pgcpp::server::InitializeWalWriter;
using pgcpp::server::InterruptFlags;
using pgcpp::server::InterruptRequested;
using pgcpp::server::IsArchiveCommandSet;
using pgcpp::server::IsInForkedProcess;
using pgcpp::server::kCheckpointCauseTime;
using pgcpp::server::kCheckpointForce;
using pgcpp::server::kCheckpointImmediate;
using pgcpp::server::kCheckpointIsShutdown;
using pgcpp::server::kCheckpointWait;
using pgcpp::server::LastCheckpointLSN;
using pgcpp::server::LookupAuxProcessType;
using pgcpp::server::LookupBgworkerName;
using pgcpp::server::PgArchiveWALFile;
using pgcpp::server::PgArchStart;
using pgcpp::server::PgArchState;
using pgcpp::server::PgArchStop;
using pgcpp::server::QueueArchiveRequest;
using pgcpp::server::RegisterAutoVacuumWorkItem;
using pgcpp::server::RegisterBackgroundWorker;
using pgcpp::server::RegisterInterruptHandler;
using pgcpp::server::ResetAutoVacuum;
using pgcpp::server::ResetBgWorker;
using pgcpp::server::ResetBgWriter;
using pgcpp::server::ResetCheckpointer;
using pgcpp::server::ResetInterruptFlags;
using pgcpp::server::ResetPgArch;
using pgcpp::server::ResetShellArchive;
using pgcpp::server::ResetStartupProcess;
using pgcpp::server::ResetSysLogger;
using pgcpp::server::ResetWalWriter;
using pgcpp::server::SetArchiveCommand;
using pgcpp::server::SetInForkedProcess;
using pgcpp::server::ShellArchive;
using pgcpp::server::ShellArchiveStats;
using pgcpp::server::StartupState;
using pgcpp::server::StartupStats;
using pgcpp::server::SysLoggerStart;
using pgcpp::server::SysLoggerState;
using pgcpp::server::SysLoggerStop;
using pgcpp::server::SysLoggerWrite;
using pgcpp::server::WalWriterStats;
using pgcpp::transaction::GetXLogInsertRecPtr;
using pgcpp::transaction::InitializeWal;
using pgcpp::transaction::kInvalidXLogRecPtr;
using pgcpp::transaction::kRmgrXactId;
using pgcpp::transaction::kSizeofXlogRecord;
using pgcpp::transaction::RegisterRmgrRedo;
using pgcpp::transaction::ResetWal;
using pgcpp::transaction::ResetXlogInsertState;
using pgcpp::transaction::XLogBeginInsert;
using pgcpp::transaction::XLogFlush;
using pgcpp::transaction::XLogInsert;
using pgcpp::transaction::XLogRecPtr;

// Buffer pool imports for bgwriter/checkpointer tests.
using pgcpp::storage::Buffer;
using pgcpp::storage::ForkNumber;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::kBlckSz;
using pgcpp::storage::MarkBufferDirty;
using pgcpp::storage::ReadBuffer;
using pgcpp::storage::ReadBufferMode;
using pgcpp::storage::ReleaseBuffer;
using pgcpp::storage::RelFileNodeBackend;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::storage::smgrcreate;
using pgcpp::storage::smgrextend;
using pgcpp::storage::smgropen;
using pgcpp::storage::SmgrRelation;

// ===========================================================================
// Test fixture — resets all auxiliary process subsystems between tests.
// ===========================================================================
class AuxProcessTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset all auxiliary process state.
        ResetInterruptFlags();
        pgcpp::server::ClearInterruptHandlers();
        InitializeBgWriter();
        InitializeCheckpointer();
        InitializeStartupProcess();
        InitializeWalWriter();
        InitializeAutoVacuum();
        InitializePgArch();
        InitializeSysLogger();
        InitializeBgWorker();
        InitializeShellArchive();
        // Reset WAL subsystem (for tests that write WAL records).
        ResetWal();
        ResetXlogInsertState();
        InitializeWal();

        // Set up a buffer pool for bgwriter/checkpointer tests.
        pgcpp::error::InitErrorSubsystem();
        mem_ctx_ = pgcpp::memory::AllocSetContext::Create("aux_process_test");
        pgcpp::memory::SetCurrentMemoryContext(mem_ctx_);
        test_dir_ = "/tmp/pgcpp_aux_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        int rc = std::system(("rm -rf " + test_dir_).c_str());
        (void)rc;
        InitBufferPool(64);
    }

    void TearDown() override {
        // Clean up signal handlers installed by tests.
        ResetInterruptFlags();
        // Shut down buffer pool (flushes dirty buffers, frees shm).
        ShutdownBufferPool();
        smgrcloseall();
        int rc = std::system(("rm -rf " + test_dir_).c_str());
        (void)rc;
    }

    // CreateDirtyBuffers — read `count` blocks into the pool and mark them
    // dirty. Used by bgwriter/checkpointer tests to set up flush targets.
    int CreateDirtyBuffers(int count) {
        RelFileNodeBackend rnode;
        rnode.node.spc_node = 0;
        rnode.node.db_node = 16384;
        rnode.node.rel_node = 200;
        rnode.backend = 0;
        SmgrRelation reln = smgropen(rnode);
        smgrcreate(reln, ForkNumber::kMain, false);

        // Extend with `count` blocks of zeroed pages.
        char buf[kBlckSz];
        std::memset(buf, 0, kBlckSz);
        pgcpp::storage::PageInit(buf, kBlckSz, 0);
        for (int i = 0; i < count; ++i) {
            smgrextend(reln, ForkNumber::kMain, i, buf, false);
        }

        // Read each block into the pool and mark it dirty, then release the
        // pin so the bgwriter can flush it (FlushDirtyBuffers skips pinned
        // buffers to avoid writing partially-updated pages).
        int created = 0;
        for (int i = 0; i < count; ++i) {
            Buffer b = ReadBuffer(reln, ForkNumber::kMain, i, ReadBufferMode::kNormal);
            if (b != 0) {
                MarkBufferDirty(b);
                pgcpp::storage::ReleaseBuffer(b);
                ++created;
            }
        }
        return created;
    }

    pgcpp::memory::MemoryContext* mem_ctx_ = nullptr;
    std::string test_dir_;
};

// ===========================================================================
// Part 1: AuxiliaryProcessType — enum, lookup, dispatch
// ===========================================================================

TEST_F(AuxProcessTest, AuxProcessTypeToString) {
    EXPECT_STREQ(AuxProcessTypeToString(AuxiliaryProcessType::kNoProcess), "none");
    EXPECT_STREQ(AuxProcessTypeToString(AuxiliaryProcessType::kBgWriter), "bgwriter");
    EXPECT_STREQ(AuxProcessTypeToString(AuxiliaryProcessType::kCheckPointer), "checkpointer");
    EXPECT_STREQ(AuxProcessTypeToString(AuxiliaryProcessType::kStartupProcess), "startup");
    EXPECT_STREQ(AuxProcessTypeToString(AuxiliaryProcessType::kWalWriter), "walwriter");
    EXPECT_STREQ(AuxProcessTypeToString(AuxiliaryProcessType::kPgArch), "archiver");
    EXPECT_STREQ(AuxProcessTypeToString(AuxiliaryProcessType::kSysLogger), "syslogger");
    EXPECT_STREQ(AuxProcessTypeToString(AuxiliaryProcessType::kBgWorker), "bgworker");
}

TEST_F(AuxProcessTest, LookupAuxProcessType) {
    EXPECT_EQ(LookupAuxProcessType("bgwriter"), AuxiliaryProcessType::kBgWriter);
    EXPECT_EQ(LookupAuxProcessType("checkpointer"), AuxiliaryProcessType::kCheckPointer);
    EXPECT_EQ(LookupAuxProcessType("startup"), AuxiliaryProcessType::kStartupProcess);
    EXPECT_EQ(LookupAuxProcessType("walwriter"), AuxiliaryProcessType::kWalWriter);
    EXPECT_EQ(LookupAuxProcessType("archiver"), AuxiliaryProcessType::kPgArch);
    EXPECT_EQ(LookupAuxProcessType("syslogger"), AuxiliaryProcessType::kSysLogger);
    EXPECT_EQ(LookupAuxProcessType("unknown"), AuxiliaryProcessType::kNoProcess);
}

TEST_F(AuxProcessTest, AuxProcessMainNoProcessReturnsError) {
    EXPECT_EQ(AuxProcessMain(AuxiliaryProcessType::kNoProcess), -1);
}

// ===========================================================================
// Part 2: Interrupt handling
// ===========================================================================

TEST_F(AuxProcessTest, InterruptFlagsInitiallyFalse) {
    EXPECT_FALSE(InterruptFlags::InterruptPending);
    EXPECT_FALSE(InterruptFlags::QueryCancelPending);
    EXPECT_FALSE(InterruptFlags::ProcDiePending);
    EXPECT_FALSE(InterruptFlags::ShutdownRequested);
}

TEST_F(AuxProcessTest, QueryCancelSignalSetsFlags) {
    pgcpp::server::HandleQueryCancelSignal(SIGINT);
    EXPECT_TRUE(InterruptFlags::QueryCancelPending);
    EXPECT_TRUE(InterruptFlags::InterruptPending);
}

TEST_F(AuxProcessTest, ShutdownSignalSetsFlags) {
    pgcpp::server::HandleShutdownSignal(SIGTERM);
    EXPECT_TRUE(InterruptFlags::ShutdownRequested);
    EXPECT_TRUE(InterruptFlags::InterruptPending);
}

TEST_F(AuxProcessTest, InterruptRequestedAfterCancel) {
    pgcpp::server::HandleQueryCancelSignal(SIGINT);
    EXPECT_TRUE(InterruptRequested());
}

TEST_F(AuxProcessTest, HandleInterruptsClearsFlags) {
    pgcpp::server::HandleQueryCancelSignal(SIGINT);
    ASSERT_TRUE(InterruptFlags::QueryCancelPending);
    HandleInterrupts();
    EXPECT_FALSE(InterruptFlags::QueryCancelPending);
    EXPECT_FALSE(InterruptFlags::InterruptPending);
}

TEST_F(AuxProcessTest, RegisterAndDispatchInterruptHandler) {
    std::atomic<int> call_count{0};
    int id = RegisterInterruptHandler("QueryCancel", [&]() { ++call_count; });
    ASSERT_GT(id, 0);

    pgcpp::server::HandleQueryCancelSignal(SIGINT);
    HandleInterrupts();
    EXPECT_EQ(call_count.load(), 1);
}

TEST_F(AuxProcessTest, UnregisterInterruptHandler) {
    std::atomic<int> call_count{0};
    int id = RegisterInterruptHandler("QueryCancel", [&]() { ++call_count; });
    pgcpp::server::UnregisterInterruptHandler(id);

    pgcpp::server::HandleQueryCancelSignal(SIGINT);
    HandleInterrupts();
    EXPECT_EQ(call_count.load(), 0);
}

TEST_F(AuxProcessTest, WaitForInterruptReturnsFalseOnTimeout) {
    EXPECT_FALSE(pgcpp::server::WaitForInterrupt(/*timeout_ms=*/10));
}

TEST_F(AuxProcessTest, WaitForInterruptReturnsTrueWhenSet) {
    // Set the flag in a separate thread after a short delay.
    std::thread setter([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pgcpp::server::HandleShutdownSignal(SIGTERM);
    });
    bool got = pgcpp::server::WaitForInterrupt(/*timeout_ms=*/1000);
    setter.join();
    EXPECT_TRUE(got);
}

// ===========================================================================
// Part 3: ForkProcess — role tracking (without actual fork)
// ===========================================================================

TEST_F(AuxProcessTest, ForkProcessRoleTracking) {
    EXPECT_FALSE(IsInForkedProcess());
    EXPECT_TRUE(pgcpp::server::GetForkedProcessRole().empty());

    SetInForkedProcess(true);
    pgcpp::server::SetForkedProcessRole("test_worker");

    EXPECT_TRUE(IsInForkedProcess());
    EXPECT_EQ(pgcpp::server::GetForkedProcessRole(), "test_worker");

    SetInForkedProcess(false);
    pgcpp::server::SetForkedProcessRole("");
    EXPECT_FALSE(IsInForkedProcess());
}

// ===========================================================================
// Part 4: BgWriter — 后台脏页刷盘
// ===========================================================================

TEST_F(AuxProcessTest, BgWriterInitialState) {
    BgWriterStats stats = GetBgWriterStats();
    EXPECT_EQ(stats.buffers_written, 0u);
    EXPECT_EQ(stats.flush_cycles, 0u);
    EXPECT_FALSE(stats.running);
}

TEST_F(AuxProcessTest, BgWriterScheduleFlushSetsTarget) {
    // Create 10 dirty buffers so FlushBuffers has something to flush.
    int created = CreateDirtyBuffers(10);
    ASSERT_EQ(created, 10);

    pgcpp::server::BgWriterScheduleFlush(/*target_count=*/10);
    // FlushBuffers drains the target, up to max_buffers.
    int flushed = pgcpp::server::BgWriterFlushBuffers(/*max_buffers=*/5);
    EXPECT_EQ(flushed, 5);
    flushed = pgcpp::server::BgWriterFlushBuffers(/*max_buffers=*/5);
    EXPECT_EQ(flushed, 5);
    // Target exhausted (all dirty buffers flushed).
    flushed = pgcpp::server::BgWriterFlushBuffers(/*max_buffers=*/5);
    EXPECT_EQ(flushed, 0);
}

TEST_F(AuxProcessTest, BgWriterFlushUpdatesStats) {
    // Create 30 dirty buffers so the flush count is reflected in stats.
    int created = CreateDirtyBuffers(30);
    ASSERT_EQ(created, 30);

    pgcpp::server::BgWriterScheduleFlush(/*target_count=*/100);
    pgcpp::server::BgWriterFlushBuffers(/*max_buffers=*/30);

    BgWriterStats stats = GetBgWriterStats();
    EXPECT_EQ(stats.buffers_written, 30u);
}

TEST_F(AuxProcessTest, BgWriterMainFlushesScheduledBuffers) {
    // Create some dirty buffers for the main loop to flush.
    int created = CreateDirtyBuffers(10);
    ASSERT_GT(created, 0);

    pgcpp::server::BgWriterScheduleFlush(/*target_count=*/100);
    int total = pgcpp::server::BgWriterMain(/*max_iterations=*/10);
    EXPECT_GT(total, 0);

    BgWriterStats stats = GetBgWriterStats();
    EXPECT_GT(stats.flush_cycles, 0u);
    EXPECT_GT(stats.last_flush_time_ms, 0);
}

TEST_F(AuxProcessTest, BgWriterMainExitsOnShutdown) {
    InterruptFlags::BgWriterShutdownRequested = true;
    pgcpp::server::BgWriterScheduleFlush(/*target_count=*/1000);
    int total = pgcpp::server::BgWriterMain(/*max_iterations=*/100);
    // Should not flush anything due to immediate shutdown.
    EXPECT_EQ(total, 0);
}

// ===========================================================================
// Part 5: Checkpointer — 检查点
// ===========================================================================

TEST_F(AuxProcessTest, CheckpointerInitialState) {
    CheckpointStats stats = GetCheckpointStats();
    EXPECT_EQ(stats.checkpoints_requested, 0u);
    EXPECT_EQ(stats.checkpoints_timed, 0u);
    EXPECT_EQ(stats.buffers_written, 0u);
    EXPECT_EQ(LastCheckpointLSN(), 0u);
    EXPECT_FALSE(CheckpointerIsRunning());
}

TEST_F(AuxProcessTest, CreateCheckPointUpdatesStatsAndLsn) {
    // Create some dirty buffers so the checkpoint has something to flush.
    int created = CreateDirtyBuffers(5);
    ASSERT_GT(created, 0);

    // Write a WAL record first so the LSN advances.
    XLogBeginInsert();
    XLogInsert(kRmgrXactId, /*info=*/0);
    XLogRecPtr lsn_before = GetXLogInsertRecPtr();
    ASSERT_GT(lsn_before, static_cast<XLogRecPtr>(kSizeofXlogRecord));

    bool ok = CreateCheckPoint(kCheckpointForce);
    EXPECT_TRUE(ok);

    CheckpointStats stats = GetCheckpointStats();
    EXPECT_EQ(stats.checkpoints_requested, 1u);
    EXPECT_GT(stats.buffers_written, 0u);
    EXPECT_GT(stats.last_checkpoint_time_ms, 0);
    EXPECT_GT(LastCheckpointLSN(), 0u);
}

TEST_F(AuxProcessTest, CreateCheckPointWithCauseTimeIncrementsTimed) {
    CreateCheckPoint(kCheckpointCauseTime);
    CheckpointStats stats = GetCheckpointStats();
    EXPECT_EQ(stats.checkpoints_timed, 1u);
    EXPECT_EQ(stats.checkpoints_requested, 0u);
}

TEST_F(AuxProcessTest, RequestCheckpointQueuesForMainLoop) {
    pgcpp::server::RequestCheckpoint(kCheckpointForce);
    int done = CheckpointerMain(/*max_iterations=*/10);
    EXPECT_EQ(done, 1);

    CheckpointStats stats = GetCheckpointStats();
    EXPECT_EQ(stats.checkpoints_requested, 1u);
}

TEST_F(AuxProcessTest, CheckpointerMainExitsOnShutdown) {
    InterruptFlags::ShutdownRequested = true;
    pgcpp::server::RequestCheckpoint(kCheckpointForce);
    int done = CheckpointerMain(/*max_iterations=*/10);
    EXPECT_EQ(done, 0);
}

TEST_F(AuxProcessTest, CheckpointerMainNoPendingExitsEarly) {
    int done = CheckpointerMain(/*max_iterations=*/10);
    EXPECT_EQ(done, 0);
}

// ===========================================================================
// Part 6: Startup — crash recovery
// ===========================================================================

TEST_F(AuxProcessTest, StartupInitialState) {
    EXPECT_EQ(GetStartupState(), StartupState::kNotStarted);
    EXPECT_FALSE(pgcpp::server::IsRecoveryInProgress());
}

TEST_F(AuxProcessTest, StartupProcessMainTransitionsState) {
    int rc = pgcpp::server::StartupProcessMain();
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(GetStartupState(), StartupState::kDone);
    EXPECT_FALSE(pgcpp::server::IsRecoveryInProgress());
}

TEST_F(AuxProcessTest, StartupReplaysWALRecords) {
    // Register a redo function that counts calls.
    std::atomic<int> redo_calls{0};
    RegisterRmgrRedo(kRmgrXactId, [&](const auto& /*rec*/, const auto* /*data*/,
                                      std::size_t /*len*/, XLogRecPtr /*lsn*/) { ++redo_calls; });

    // Write 3 WAL records.
    for (int i = 0; i < 3; ++i) {
        XLogBeginInsert();
        XLogInsert(kRmgrXactId, /*info=*/0);
    }

    int rc = pgcpp::server::StartupProcessMain();
    EXPECT_EQ(rc, 0);

    StartupStats stats = GetStartupStats();
    EXPECT_EQ(stats.records_replayed, 3u);
    EXPECT_EQ(redo_calls.load(), 3);
    EXPECT_GT(stats.recovery_end_lsn, 0u);
}

TEST_F(AuxProcessTest, StartupSkipsUnregisteredRmgrs) {
    // Write a record with an RMGR that has no redo function registered.
    XLogBeginInsert();
    XLogInsert(/*rmid=*/200, /*info=*/0);

    int rc = pgcpp::server::StartupProcessMain();
    EXPECT_EQ(rc, 0);

    StartupStats stats = GetStartupStats();
    EXPECT_EQ(stats.records_replayed, 0u);
    EXPECT_EQ(stats.records_skipped, 1u);
}

TEST_F(AuxProcessTest, SetRecoveryStartLsnChangesStartPoint) {
    // Write 2 records. XLogInsert returns the start LSN of the inserted record;
    // to skip the first record we must start recovery from the second record's
    // start LSN (which is the value returned by the second XLogInsert call).
    XLogBeginInsert();
    XLogInsert(kRmgrXactId, /*info=*/0);  // First record.
    XLogBeginInsert();
    XLogRecPtr second_lsn = XLogInsert(kRmgrXactId, /*info=*/0);  // Second record.

    // Register a redo function that counts calls.
    std::atomic<int> redo_calls{0};
    RegisterRmgrRedo(kRmgrXactId, [&](const auto& /*rec*/, const auto* /*data*/,
                                      std::size_t /*len*/, XLogRecPtr /*lsn*/) { ++redo_calls; });

    // Start recovery from the second record — should replay only 1.
    pgcpp::server::SetRecoveryStartLsn(second_lsn);
    int rc = pgcpp::server::StartupProcessMain();
    EXPECT_EQ(rc, 0);

    StartupStats stats = GetStartupStats();
    EXPECT_EQ(stats.records_replayed, 1u);
    EXPECT_EQ(redo_calls.load(), 1);
}

// ===========================================================================
// Part 7: WalWriter — WAL 写入
// ===========================================================================

TEST_F(AuxProcessTest, WalWriterInitialState) {
    WalWriterStats stats = GetWalWriterStats();
    EXPECT_EQ(stats.bytes_written, 0u);
    EXPECT_EQ(stats.flush_cycles, 0u);
    EXPECT_FALSE(stats.running);
}

TEST_F(AuxProcessTest, WalWriterFlushReportsBytesWritten) {
    // Write some WAL first.
    XLogBeginInsert();
    XLogInsert(kRmgrXactId, /*info=*/0);

    uint64_t bytes = pgcpp::server::WalWriterFlush();
    EXPECT_GT(bytes, 0u);

    WalWriterStats stats = GetWalWriterStats();
    EXPECT_GT(stats.bytes_written, 0u);
    EXPECT_GT(stats.last_flush_lsn, 0u);
}

TEST_F(AuxProcessTest, WalWriterFlushNoNewWALReturnsZero) {
    // No WAL written — first flush moves from 0 to kSizeofXlogRecord (24)
    // because InitializeWal advances the insert pointer past the header.
    uint64_t bytes = pgcpp::server::WalWriterFlush();
    // The first flush always sees at least the WAL header area as "new".
    // After that, subsequent flushes return 0.
    bytes = pgcpp::server::WalWriterFlush();
    EXPECT_EQ(bytes, 0u);
}

TEST_F(AuxProcessTest, WalWriterMainRunsCycles) {
    XLogBeginInsert();
    XLogInsert(kRmgrXactId, /*info=*/0);

    int cycles = pgcpp::server::WalWriterMain(/*max_iterations=*/5);
    EXPECT_GT(cycles, 0);

    WalWriterStats stats = GetWalWriterStats();
    EXPECT_FALSE(stats.running);  // Should be false after main returns.
}

TEST_F(AuxProcessTest, WalWriterMainExitsOnShutdown) {
    InterruptFlags::WalWriterShutdownRequested = true;
    int cycles = pgcpp::server::WalWriterMain(/*max_iterations=*/5);
    EXPECT_EQ(cycles, 0);
}

// ===========================================================================
// Part 8: AutoVacuum — 自动 VACUUM
// ===========================================================================

TEST_F(AuxProcessTest, AutoVacuumRegisterWorkItem) {
    AutoVacuumWorkItem item;
    item.database = "testdb";
    item.table = "public.users";
    item.is_vacuum = true;

    EXPECT_TRUE(RegisterAutoVacuumWorkItem(item));
    auto pending = pgcpp::server::GetPendingAutoVacuumItems();
    EXPECT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].database, "testdb");
    EXPECT_EQ(pending[0].table, "public.users");
}

TEST_F(AuxProcessTest, AutoVacuumDuplicateRejected) {
    AutoVacuumWorkItem item;
    item.database = "testdb";
    item.table = "public.users";

    EXPECT_TRUE(RegisterAutoVacuumWorkItem(item));
    EXPECT_FALSE(RegisterAutoVacuumWorkItem(item));  // Duplicate.
    EXPECT_EQ(pgcpp::server::GetPendingAutoVacuumItems().size(), 1u);
}

TEST_F(AuxProcessTest, AutoVacuumLauncherMainProcessesQueue) {
    RegisterAutoVacuumWorkItem({"testdb", "public.t1", false, true, 0});
    RegisterAutoVacuumWorkItem({"testdb", "public.t2", true, false, 0});
    RegisterAutoVacuumWorkItem({"testdb", "public.t3", false, true, 0});

    int launched = pgcpp::server::AutoVacuumLauncherMain(/*max_workers=*/10);
    EXPECT_EQ(launched, 3);

    AutoVacuumStats stats = pgcpp::server::GetAutoVacuumStats();
    EXPECT_EQ(stats.workers_launched, 3u);
    EXPECT_EQ(stats.workers_completed, 3u);
    EXPECT_EQ(stats.vacuums_run, 2u);
    EXPECT_EQ(stats.analyzes_run, 1u);
    EXPECT_EQ(pgcpp::server::GetPendingAutoVacuumItems().size(), 0u);
}

TEST_F(AuxProcessTest, AutoVacuumWorkerRejectsEmptyNames) {
    AutoVacuumWorkItem empty;
    EXPECT_NE(pgcpp::server::AutoVacuumWorkerMain(empty), 0);

    AutoVacuumWorkItem valid;
    valid.database = "db";
    valid.table = "t";
    EXPECT_EQ(pgcpp::server::AutoVacuumWorkerMain(valid), 0);
}

TEST_F(AuxProcessTest, AutoVacuumLauncherExitsOnShutdown) {
    RegisterAutoVacuumWorkItem({"db", "t", false, true, 0});
    InterruptFlags::ShutdownRequested = true;
    int launched = pgcpp::server::AutoVacuumLauncherMain(/*max_workers=*/10);
    EXPECT_EQ(launched, 0);
}

// ===========================================================================
// Part 9: PgArch + ShellArchive — 归档
// ===========================================================================

TEST_F(AuxProcessTest, ShellArchiveNoCommandReturnsZero) {
    EXPECT_FALSE(IsArchiveCommandSet());
    EXPECT_EQ(ShellArchive("/tmp/test_wal", /*last=*/""), 0);
}

TEST_F(AuxProcessTest, ShellArchiveSubstitutesPlaceholders) {
    // Use a command that always succeeds: `true`.
    SetArchiveCommand("true");
    ASSERT_TRUE(IsArchiveCommandSet());
    EXPECT_EQ(ShellArchive("/tmp/test_wal_001", /*last=*/""), 0);

    ShellArchiveStats stats = GetShellArchiveStats();
    EXPECT_EQ(stats.files_archived, 1u);
    EXPECT_EQ(stats.failures, 0u);
    EXPECT_EQ(stats.last_exit_code, 0);
    EXPECT_EQ(stats.last_archived_file, "/tmp/test_wal_001");
}

TEST_F(AuxProcessTest, ShellArchiveFailureIncrementsFailures) {
    // Use a command that always fails: `false`.
    SetArchiveCommand("false");
    EXPECT_NE(ShellArchive("/tmp/test_wal_002", /*last=*/""), 0);

    ShellArchiveStats stats = GetShellArchiveStats();
    EXPECT_EQ(stats.files_archived, 0u);
    EXPECT_EQ(stats.failures, 1u);
    EXPECT_NE(stats.last_exit_code, 0);
}

TEST_F(AuxProcessTest, ShellArchiveSubstitutesPathAndBasename) {
    // Echo the substituted path to a temp file, then verify its contents.
    std::string marker_file = "/tmp/pgcpp_shell_archive_test_" + std::to_string(getpid()) + ".txt";
    ::unlink(marker_file.c_str());

    // Command: write the substituted path to the marker file.
    std::string cmd = "echo '%p %f' > " + marker_file;
    SetArchiveCommand(cmd);

    EXPECT_EQ(ShellArchive("/var/lib/wal/000000010000000000000001", /*last=*/""), 0);

    // Read the marker file and verify.
    FILE* f = fopen(marker_file.c_str(), "r");
    ASSERT_NE(f, nullptr);
    char buf[256];
    if (fgets(buf, sizeof(buf), f) == nullptr)
        buf[0] = '\0';
    fclose(f);
    ::unlink(marker_file.c_str());

    std::string content(buf);
    EXPECT_NE(content.find("/var/lib/wal/000000010000000000000001"), std::string::npos);
    EXPECT_NE(content.find("000000010000000000000001"), std::string::npos);
}

TEST_F(AuxProcessTest, PgArchInitialState) {
    EXPECT_EQ(GetPgArchState(), PgArchState::kStopped);
    auto stats = GetPgArchStats();
    EXPECT_EQ(stats.files_archived, 0u);
    EXPECT_FALSE(stats.running);
}

TEST_F(AuxProcessTest, PgArchStartStopTransitionsState) {
    PgArchStart();
    EXPECT_EQ(GetPgArchState(), PgArchState::kRunning);
    EXPECT_TRUE(GetPgArchStats().running);

    PgArchStop();
    EXPECT_EQ(GetPgArchState(), PgArchState::kStopped);
    EXPECT_FALSE(GetPgArchStats().running);
}

TEST_F(AuxProcessTest, PgArchiveWALFileSuccess) {
    SetArchiveCommand("true");
    EXPECT_TRUE(PgArchiveWALFile("/tmp/wal_001"));

    auto stats = GetPgArchStats();
    EXPECT_EQ(stats.files_archived, 1u);
    EXPECT_EQ(stats.archive_failures, 0u);
    EXPECT_EQ(stats.last_archived_file, "/tmp/wal_001");
}

TEST_F(AuxProcessTest, PgArchiveWALFileFailure) {
    SetArchiveCommand("false");
    EXPECT_FALSE(PgArchiveWALFile("/tmp/wal_002"));

    auto stats = GetPgArchStats();
    EXPECT_EQ(stats.files_archived, 0u);
    EXPECT_EQ(stats.archive_failures, 1u);
}

TEST_F(AuxProcessTest, QueueArchiveRequestDeduplicates) {
    EXPECT_TRUE(QueueArchiveRequest("/tmp/wal_003"));
    EXPECT_FALSE(QueueArchiveRequest("/tmp/wal_003"));
    EXPECT_EQ(GetPendingArchiveRequests().size(), 1u);
}

TEST_F(AuxProcessTest, PgArchiverMainProcessesQueue) {
    SetArchiveCommand("true");
    QueueArchiveRequest("/tmp/wal_004");
    QueueArchiveRequest("/tmp/wal_005");
    PgArchStart();

    int archived = pgcpp::server::PgArchiverMain(/*max_iterations=*/10);
    EXPECT_EQ(archived, 2);
    EXPECT_EQ(GetPendingArchiveRequests().size(), 0u);

    auto stats = GetPgArchStats();
    EXPECT_EQ(stats.files_archived, 2u);
}

TEST_F(AuxProcessTest, PgArchiverMainRequeuesOnFailure) {
    SetArchiveCommand("false");
    QueueArchiveRequest("/tmp/wal_006");
    PgArchStart();

    int archived = pgcpp::server::PgArchiverMain(/*max_iterations=*/10);
    EXPECT_EQ(archived, 0);
    // Failed file should be re-queued for retry.
    EXPECT_EQ(GetPendingArchiveRequests().size(), 1u);

    auto stats = GetPgArchStats();
    EXPECT_EQ(stats.archive_failures, 1u);
}

// ===========================================================================
// Part 10: SysLogger
// ===========================================================================

TEST_F(AuxProcessTest, SysLoggerInitialState) {
    EXPECT_EQ(GetSysLoggerState(), SysLoggerState::kStopped);
    auto stats = GetSysLoggerStats();
    EXPECT_EQ(stats.messages_logged, 0u);
    EXPECT_FALSE(stats.running);
}

TEST_F(AuxProcessTest, SysLoggerStartStopTransitionsState) {
    SysLoggerStart();
    EXPECT_EQ(GetSysLoggerState(), SysLoggerState::kRunning);

    SysLoggerStop();
    EXPECT_EQ(GetSysLoggerState(), SysLoggerState::kStopped);
}

TEST_F(AuxProcessTest, SysLoggerWriteQueuesMessage) {
    SysLoggerStart();
    SysLoggerWrite("INFO", "hello world");
    SysLoggerWrite("ERROR", "something broke");

    int processed = pgcpp::server::SysLoggerMain(/*max_iterations=*/10);
    EXPECT_EQ(processed, 2);

    auto messages = GetSysLoggerMessages();
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].level, "INFO");
    EXPECT_EQ(messages[0].message, "hello world");
    EXPECT_EQ(messages[1].level, "ERROR");
    EXPECT_EQ(messages[1].message, "something broke");
}

TEST_F(AuxProcessTest, SysLoggerStatsByLevel) {
    SysLoggerStart();
    SysLoggerWrite("DEBUG", "d");
    SysLoggerWrite("INFO", "i");
    SysLoggerWrite("NOTICE", "n");
    SysLoggerWrite("WARNING", "w");
    SysLoggerWrite("ERROR", "e");
    SysLoggerWrite("FATAL", "f");

    pgcpp::server::SysLoggerMain(/*max_iterations=*/10);

    auto stats = GetSysLoggerStats();
    EXPECT_EQ(stats.debug_count, 1u);
    EXPECT_EQ(stats.info_count, 1u);
    EXPECT_EQ(stats.notice_count, 1u);
    EXPECT_EQ(stats.warning_count, 1u);
    EXPECT_EQ(stats.error_count, 1u);
    EXPECT_EQ(stats.fatal_count, 1u);
    EXPECT_EQ(stats.messages_logged, 6u);
    EXPECT_GT(stats.bytes_written, 0u);
}

TEST_F(AuxProcessTest, SysLoggerMainNoOpWhenStopped) {
    SysLoggerWrite("INFO", "should not process");
    int processed = pgcpp::server::SysLoggerMain(/*max_iterations=*/10);
    EXPECT_EQ(processed, 0);
}

// ===========================================================================
// Part 11: BgWorker — registry, launch, terminate
// ===========================================================================

TEST_F(AuxProcessTest, BgWorkerRegister) {
    BackgroundWorker w;
    w.name = "test_worker";
    w.type = BgWorkerType::kDynamic;
    w.main_fn = []() {};

    int id = RegisterBackgroundWorker(w);
    ASSERT_GE(id, 0);
    EXPECT_EQ(LookupBgworkerName("test_worker"), id);
}

TEST_F(AuxProcessTest, BgWorkerDuplicateNameRejected) {
    BackgroundWorker w;
    w.name = "dup";
    w.main_fn = []() {};

    ASSERT_GE(RegisterBackgroundWorker(w), 0);
    EXPECT_EQ(RegisterBackgroundWorker(w), -1);
}

TEST_F(AuxProcessTest, BgWorkerLookupNotFound) {
    EXPECT_EQ(LookupBgworkerName("nonexistent"), -1);
}

TEST_F(AuxProcessTest, BgWorkerLaunchInvokesMain) {
    std::atomic<int> call_count{0};
    BackgroundWorker w;
    w.name = "runnable";
    w.main_fn = [&]() { ++call_count; };

    int id = RegisterBackgroundWorker(w);
    ASSERT_GE(id, 0);
    EXPECT_EQ(GetBgWorkerState(id), BgWorkerState::kRegistered);

    EXPECT_TRUE(pgcpp::server::LaunchBackgroundWorker(id));
    EXPECT_EQ(call_count.load(), 1);
    EXPECT_EQ(GetBgWorkerState(id), BgWorkerState::kStopped);
}

TEST_F(AuxProcessTest, BgWorkerMainDispatches) {
    std::atomic<int> call_count{0};
    BackgroundWorker w;
    w.name = "main_dispatch_test";
    w.main_fn = [&]() { ++call_count; };

    int id = RegisterBackgroundWorker(w);
    int rc = pgcpp::server::BgWorkerMain(id);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(call_count.load(), 1);
}

TEST_F(AuxProcessTest, BgWorkerTerminateRegisteredWorker) {
    BackgroundWorker w;
    w.name = "terminable";
    w.main_fn = []() {};

    int id = RegisterBackgroundWorker(w);
    EXPECT_TRUE(pgcpp::server::TerminateBackgroundWorker(id));
    EXPECT_EQ(GetBgWorkerState(id), BgWorkerState::kStopped);
}

TEST_F(AuxProcessTest, BgWorkerGetBackgroundWorker) {
    BackgroundWorker w;
    w.name = "gettable";
    w.type = BgWorkerType::kBackend;
    w.main_fn = []() {};

    int id = RegisterBackgroundWorker(w);
    BackgroundWorker retrieved;
    EXPECT_TRUE(pgcpp::server::GetBackgroundWorker(id, &retrieved));
    EXPECT_EQ(retrieved.name, "gettable");
    EXPECT_EQ(retrieved.type, BgWorkerType::kBackend);
}

TEST_F(AuxProcessTest, BgWorkerGetBackgroundWorkerInvalidId) {
    BackgroundWorker w;
    EXPECT_FALSE(pgcpp::server::GetBackgroundWorker(999, &w));
}

TEST_F(AuxProcessTest, BgWorkerLaunchInvalidIdReturnsFalse) {
    EXPECT_FALSE(pgcpp::server::LaunchBackgroundWorker(999));
}

// ===========================================================================
// Part 12: AuxProcessMain integration
// ===========================================================================

TEST_F(AuxProcessTest, AuxProcessMainBgWriter) {
    // Schedule some flush work so BgWriterMain does something.
    pgcpp::server::BgWriterScheduleFlush(/*target_count=*/5);
    int rc = AuxProcessMain(AuxiliaryProcessType::kBgWriter);
    EXPECT_GE(rc, 0);
}

TEST_F(AuxProcessTest, AuxProcessMainWalWriter) {
    int rc = AuxProcessMain(AuxiliaryProcessType::kWalWriter);
    EXPECT_GE(rc, 0);
}

TEST_F(AuxProcessTest, AuxProcessMainSysLogger) {
    SysLoggerStart();
    SysLoggerWrite("INFO", "from aux main");
    int rc = AuxProcessMain(AuxiliaryProcessType::kSysLogger);
    EXPECT_GE(rc, 0);
}

TEST_F(AuxProcessTest, AuxProcessMainStartup) {
    int rc = AuxProcessMain(AuxiliaryProcessType::kStartupProcess);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(GetStartupState(), StartupState::kDone);
}
