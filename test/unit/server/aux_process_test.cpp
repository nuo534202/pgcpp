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
#include <string>
#include <thread>
#include <vector>

#include "pgcpp/server/autovacuum.hpp"
#include "pgcpp/server/auxprocess.hpp"
#include "pgcpp/server/bgworker.hpp"
#include "pgcpp/server/bgwriter.hpp"
#include "pgcpp/server/checkpointer.hpp"
#include "pgcpp/server/fork_process.hpp"
#include "pgcpp/server/interrupt.hpp"
#include "pgcpp/server/pgarch.hpp"
#include "pgcpp/server/shell_archive.hpp"
#include "pgcpp/server/startup.hpp"
#include "pgcpp/server/syslogger.hpp"
#include "pgcpp/server/walwriter.hpp"
#include "pgcpp/transaction/xlog.hpp"
#include "pgcpp/transaction/xloginsert.hpp"
#include "pgcpp/transaction/xlogrecovery.hpp"

using mytoydb::server::AutoVacuumStats;
using mytoydb::server::AutoVacuumWorkItem;
using mytoydb::server::AuxiliaryProcessType;
using mytoydb::server::AuxProcessMain;
using mytoydb::server::AuxProcessTypeToString;
using mytoydb::server::BackgroundWorker;
using mytoydb::server::BgWorkerState;
using mytoydb::server::BgWorkerType;
using mytoydb::server::BgWriterStats;
using mytoydb::server::CheckpointerIsRunning;
using mytoydb::server::CheckpointerMain;
using mytoydb::server::CheckpointStats;
using mytoydb::server::CloseStdio;
using mytoydb::server::CreateCheckPoint;
using mytoydb::server::GetBgWorkerState;
using mytoydb::server::GetBgWriterStats;
using mytoydb::server::GetCheckpointStats;
using mytoydb::server::GetPendingArchiveRequests;
using mytoydb::server::GetPgArchState;
using mytoydb::server::GetPgArchStats;
using mytoydb::server::GetShellArchiveStats;
using mytoydb::server::GetStartupState;
using mytoydb::server::GetStartupStats;
using mytoydb::server::GetSysLoggerMessages;
using mytoydb::server::GetSysLoggerState;
using mytoydb::server::GetSysLoggerStats;
using mytoydb::server::GetWalWriterStats;
using mytoydb::server::HandleInterrupts;
using mytoydb::server::InitializeAutoVacuum;
using mytoydb::server::InitializeBgWorker;
using mytoydb::server::InitializeBgWriter;
using mytoydb::server::InitializeCheckpointer;
using mytoydb::server::InitializePgArch;
using mytoydb::server::InitializeShellArchive;
using mytoydb::server::InitializeStartupProcess;
using mytoydb::server::InitializeSysLogger;
using mytoydb::server::InitializeWalWriter;
using mytoydb::server::InterruptFlags;
using mytoydb::server::InterruptRequested;
using mytoydb::server::IsArchiveCommandSet;
using mytoydb::server::IsInForkedProcess;
using mytoydb::server::kCheckpointCauseTime;
using mytoydb::server::kCheckpointForce;
using mytoydb::server::kCheckpointImmediate;
using mytoydb::server::kCheckpointIsShutdown;
using mytoydb::server::kCheckpointWait;
using mytoydb::server::LastCheckpointLSN;
using mytoydb::server::LookupAuxProcessType;
using mytoydb::server::LookupBgworkerName;
using mytoydb::server::PgArchiveWALFile;
using mytoydb::server::PgArchStart;
using mytoydb::server::PgArchState;
using mytoydb::server::PgArchStop;
using mytoydb::server::QueueArchiveRequest;
using mytoydb::server::RegisterAutoVacuumWorkItem;
using mytoydb::server::RegisterBackgroundWorker;
using mytoydb::server::RegisterInterruptHandler;
using mytoydb::server::ResetAutoVacuum;
using mytoydb::server::ResetBgWorker;
using mytoydb::server::ResetBgWriter;
using mytoydb::server::ResetCheckpointer;
using mytoydb::server::ResetInterruptFlags;
using mytoydb::server::ResetPgArch;
using mytoydb::server::ResetShellArchive;
using mytoydb::server::ResetStartupProcess;
using mytoydb::server::ResetSysLogger;
using mytoydb::server::ResetWalWriter;
using mytoydb::server::SetArchiveCommand;
using mytoydb::server::SetInForkedProcess;
using mytoydb::server::ShellArchive;
using mytoydb::server::ShellArchiveStats;
using mytoydb::server::StartupState;
using mytoydb::server::StartupStats;
using mytoydb::server::SysLoggerStart;
using mytoydb::server::SysLoggerState;
using mytoydb::server::SysLoggerStop;
using mytoydb::server::SysLoggerWrite;
using mytoydb::server::WalWriterStats;
using mytoydb::transaction::GetXLogInsertRecPtr;
using mytoydb::transaction::InitializeWal;
using mytoydb::transaction::kInvalidXLogRecPtr;
using mytoydb::transaction::kRmgrXactId;
using mytoydb::transaction::kSizeofXlogRecord;
using mytoydb::transaction::RegisterRmgrRedo;
using mytoydb::transaction::ResetWal;
using mytoydb::transaction::ResetXlogInsertState;
using mytoydb::transaction::XLogBeginInsert;
using mytoydb::transaction::XLogFlush;
using mytoydb::transaction::XLogInsert;
using mytoydb::transaction::XLogRecPtr;

// ===========================================================================
// Test fixture — resets all auxiliary process subsystems between tests.
// ===========================================================================
class AuxProcessTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset all auxiliary process state.
        ResetInterruptFlags();
        mytoydb::server::ClearInterruptHandlers();
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
    }

    void TearDown() override {
        // Clean up signal handlers installed by tests.
        ResetInterruptFlags();
    }
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
    mytoydb::server::HandleQueryCancelSignal(SIGINT);
    EXPECT_TRUE(InterruptFlags::QueryCancelPending);
    EXPECT_TRUE(InterruptFlags::InterruptPending);
}

TEST_F(AuxProcessTest, ShutdownSignalSetsFlags) {
    mytoydb::server::HandleShutdownSignal(SIGTERM);
    EXPECT_TRUE(InterruptFlags::ShutdownRequested);
    EXPECT_TRUE(InterruptFlags::InterruptPending);
}

TEST_F(AuxProcessTest, InterruptRequestedAfterCancel) {
    mytoydb::server::HandleQueryCancelSignal(SIGINT);
    EXPECT_TRUE(InterruptRequested());
}

TEST_F(AuxProcessTest, HandleInterruptsClearsFlags) {
    mytoydb::server::HandleQueryCancelSignal(SIGINT);
    ASSERT_TRUE(InterruptFlags::QueryCancelPending);
    HandleInterrupts();
    EXPECT_FALSE(InterruptFlags::QueryCancelPending);
    EXPECT_FALSE(InterruptFlags::InterruptPending);
}

TEST_F(AuxProcessTest, RegisterAndDispatchInterruptHandler) {
    std::atomic<int> call_count{0};
    int id = RegisterInterruptHandler("QueryCancel", [&]() { ++call_count; });
    ASSERT_GT(id, 0);

    mytoydb::server::HandleQueryCancelSignal(SIGINT);
    HandleInterrupts();
    EXPECT_EQ(call_count.load(), 1);
}

TEST_F(AuxProcessTest, UnregisterInterruptHandler) {
    std::atomic<int> call_count{0};
    int id = RegisterInterruptHandler("QueryCancel", [&]() { ++call_count; });
    mytoydb::server::UnregisterInterruptHandler(id);

    mytoydb::server::HandleQueryCancelSignal(SIGINT);
    HandleInterrupts();
    EXPECT_EQ(call_count.load(), 0);
}

TEST_F(AuxProcessTest, WaitForInterruptReturnsFalseOnTimeout) {
    EXPECT_FALSE(mytoydb::server::WaitForInterrupt(/*timeout_ms=*/10));
}

TEST_F(AuxProcessTest, WaitForInterruptReturnsTrueWhenSet) {
    // Set the flag in a separate thread after a short delay.
    std::thread setter([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        mytoydb::server::HandleShutdownSignal(SIGTERM);
    });
    bool got = mytoydb::server::WaitForInterrupt(/*timeout_ms=*/1000);
    setter.join();
    EXPECT_TRUE(got);
}

// ===========================================================================
// Part 3: ForkProcess — role tracking (without actual fork)
// ===========================================================================

TEST_F(AuxProcessTest, ForkProcessRoleTracking) {
    EXPECT_FALSE(IsInForkedProcess());
    EXPECT_TRUE(mytoydb::server::GetForkedProcessRole().empty());

    SetInForkedProcess(true);
    mytoydb::server::SetForkedProcessRole("test_worker");

    EXPECT_TRUE(IsInForkedProcess());
    EXPECT_EQ(mytoydb::server::GetForkedProcessRole(), "test_worker");

    SetInForkedProcess(false);
    mytoydb::server::SetForkedProcessRole("");
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
    mytoydb::server::BgWriterScheduleFlush(/*target_count=*/10);
    // FlushBuffers drains the target.
    int flushed = mytoydb::server::BgWriterFlushBuffers(/*max_buffers=*/5);
    EXPECT_EQ(flushed, 5);
    flushed = mytoydb::server::BgWriterFlushBuffers(/*max_buffers=*/5);
    EXPECT_EQ(flushed, 5);
    // Target exhausted.
    flushed = mytoydb::server::BgWriterFlushBuffers(/*max_buffers=*/5);
    EXPECT_EQ(flushed, 0);
}

TEST_F(AuxProcessTest, BgWriterFlushUpdatesStats) {
    mytoydb::server::BgWriterScheduleFlush(/*target_count=*/100);
    mytoydb::server::BgWriterFlushBuffers(/*max_buffers=*/30);

    BgWriterStats stats = GetBgWriterStats();
    EXPECT_EQ(stats.buffers_written, 30u);
}

TEST_F(AuxProcessTest, BgWriterMainFlushesScheduledBuffers) {
    mytoydb::server::BgWriterScheduleFlush(/*target_count=*/100);
    int total = mytoydb::server::BgWriterMain(/*max_iterations=*/10);
    EXPECT_GT(total, 0);

    BgWriterStats stats = GetBgWriterStats();
    EXPECT_GT(stats.flush_cycles, 0u);
    EXPECT_GT(stats.last_flush_time_ms, 0);
}

TEST_F(AuxProcessTest, BgWriterMainExitsOnShutdown) {
    InterruptFlags::BgWriterShutdownRequested = true;
    mytoydb::server::BgWriterScheduleFlush(/*target_count=*/1000);
    int total = mytoydb::server::BgWriterMain(/*max_iterations=*/100);
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
    mytoydb::server::RequestCheckpoint(kCheckpointForce);
    int done = CheckpointerMain(/*max_iterations=*/10);
    EXPECT_EQ(done, 1);

    CheckpointStats stats = GetCheckpointStats();
    EXPECT_EQ(stats.checkpoints_requested, 1u);
}

TEST_F(AuxProcessTest, CheckpointerMainExitsOnShutdown) {
    InterruptFlags::ShutdownRequested = true;
    mytoydb::server::RequestCheckpoint(kCheckpointForce);
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
    EXPECT_FALSE(mytoydb::server::IsRecoveryInProgress());
}

TEST_F(AuxProcessTest, StartupProcessMainTransitionsState) {
    int rc = mytoydb::server::StartupProcessMain();
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(GetStartupState(), StartupState::kDone);
    EXPECT_FALSE(mytoydb::server::IsRecoveryInProgress());
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

    int rc = mytoydb::server::StartupProcessMain();
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

    int rc = mytoydb::server::StartupProcessMain();
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
    mytoydb::server::SetRecoveryStartLsn(second_lsn);
    int rc = mytoydb::server::StartupProcessMain();
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

    uint64_t bytes = mytoydb::server::WalWriterFlush();
    EXPECT_GT(bytes, 0u);

    WalWriterStats stats = GetWalWriterStats();
    EXPECT_GT(stats.bytes_written, 0u);
    EXPECT_GT(stats.last_flush_lsn, 0u);
}

TEST_F(AuxProcessTest, WalWriterFlushNoNewWALReturnsZero) {
    // No WAL written — first flush moves from 0 to kSizeofXlogRecord (24)
    // because InitializeWal advances the insert pointer past the header.
    uint64_t bytes = mytoydb::server::WalWriterFlush();
    // The first flush always sees at least the WAL header area as "new".
    // After that, subsequent flushes return 0.
    bytes = mytoydb::server::WalWriterFlush();
    EXPECT_EQ(bytes, 0u);
}

TEST_F(AuxProcessTest, WalWriterMainRunsCycles) {
    XLogBeginInsert();
    XLogInsert(kRmgrXactId, /*info=*/0);

    int cycles = mytoydb::server::WalWriterMain(/*max_iterations=*/5);
    EXPECT_GT(cycles, 0);

    WalWriterStats stats = GetWalWriterStats();
    EXPECT_FALSE(stats.running);  // Should be false after main returns.
}

TEST_F(AuxProcessTest, WalWriterMainExitsOnShutdown) {
    InterruptFlags::WalWriterShutdownRequested = true;
    int cycles = mytoydb::server::WalWriterMain(/*max_iterations=*/5);
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
    auto pending = mytoydb::server::GetPendingAutoVacuumItems();
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
    EXPECT_EQ(mytoydb::server::GetPendingAutoVacuumItems().size(), 1u);
}

TEST_F(AuxProcessTest, AutoVacuumLauncherMainProcessesQueue) {
    RegisterAutoVacuumWorkItem({"testdb", "public.t1", false, true, 0});
    RegisterAutoVacuumWorkItem({"testdb", "public.t2", true, false, 0});
    RegisterAutoVacuumWorkItem({"testdb", "public.t3", false, true, 0});

    int launched = mytoydb::server::AutoVacuumLauncherMain(/*max_workers=*/10);
    EXPECT_EQ(launched, 3);

    AutoVacuumStats stats = mytoydb::server::GetAutoVacuumStats();
    EXPECT_EQ(stats.workers_launched, 3u);
    EXPECT_EQ(stats.workers_completed, 3u);
    EXPECT_EQ(stats.vacuums_run, 2u);
    EXPECT_EQ(stats.analyzes_run, 1u);
    EXPECT_EQ(mytoydb::server::GetPendingAutoVacuumItems().size(), 0u);
}

TEST_F(AuxProcessTest, AutoVacuumWorkerRejectsEmptyNames) {
    AutoVacuumWorkItem empty;
    EXPECT_NE(mytoydb::server::AutoVacuumWorkerMain(empty), 0);

    AutoVacuumWorkItem valid;
    valid.database = "db";
    valid.table = "t";
    EXPECT_EQ(mytoydb::server::AutoVacuumWorkerMain(valid), 0);
}

TEST_F(AuxProcessTest, AutoVacuumLauncherExitsOnShutdown) {
    RegisterAutoVacuumWorkItem({"db", "t", false, true, 0});
    InterruptFlags::ShutdownRequested = true;
    int launched = mytoydb::server::AutoVacuumLauncherMain(/*max_workers=*/10);
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
    std::string marker_file =
        "/tmp/mytoydb_shell_archive_test_" + std::to_string(getpid()) + ".txt";
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

    int archived = mytoydb::server::PgArchiverMain(/*max_iterations=*/10);
    EXPECT_EQ(archived, 2);
    EXPECT_EQ(GetPendingArchiveRequests().size(), 0u);

    auto stats = GetPgArchStats();
    EXPECT_EQ(stats.files_archived, 2u);
}

TEST_F(AuxProcessTest, PgArchiverMainRequeuesOnFailure) {
    SetArchiveCommand("false");
    QueueArchiveRequest("/tmp/wal_006");
    PgArchStart();

    int archived = mytoydb::server::PgArchiverMain(/*max_iterations=*/10);
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

    int processed = mytoydb::server::SysLoggerMain(/*max_iterations=*/10);
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

    mytoydb::server::SysLoggerMain(/*max_iterations=*/10);

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
    int processed = mytoydb::server::SysLoggerMain(/*max_iterations=*/10);
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

    EXPECT_TRUE(mytoydb::server::LaunchBackgroundWorker(id));
    EXPECT_EQ(call_count.load(), 1);
    EXPECT_EQ(GetBgWorkerState(id), BgWorkerState::kStopped);
}

TEST_F(AuxProcessTest, BgWorkerMainDispatches) {
    std::atomic<int> call_count{0};
    BackgroundWorker w;
    w.name = "main_dispatch_test";
    w.main_fn = [&]() { ++call_count; };

    int id = RegisterBackgroundWorker(w);
    int rc = mytoydb::server::BgWorkerMain(id);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(call_count.load(), 1);
}

TEST_F(AuxProcessTest, BgWorkerTerminateRegisteredWorker) {
    BackgroundWorker w;
    w.name = "terminable";
    w.main_fn = []() {};

    int id = RegisterBackgroundWorker(w);
    EXPECT_TRUE(mytoydb::server::TerminateBackgroundWorker(id));
    EXPECT_EQ(GetBgWorkerState(id), BgWorkerState::kStopped);
}

TEST_F(AuxProcessTest, BgWorkerGetBackgroundWorker) {
    BackgroundWorker w;
    w.name = "gettable";
    w.type = BgWorkerType::kBackend;
    w.main_fn = []() {};

    int id = RegisterBackgroundWorker(w);
    BackgroundWorker retrieved;
    EXPECT_TRUE(mytoydb::server::GetBackgroundWorker(id, &retrieved));
    EXPECT_EQ(retrieved.name, "gettable");
    EXPECT_EQ(retrieved.type, BgWorkerType::kBackend);
}

TEST_F(AuxProcessTest, BgWorkerGetBackgroundWorkerInvalidId) {
    BackgroundWorker w;
    EXPECT_FALSE(mytoydb::server::GetBackgroundWorker(999, &w));
}

TEST_F(AuxProcessTest, BgWorkerLaunchInvalidIdReturnsFalse) {
    EXPECT_FALSE(mytoydb::server::LaunchBackgroundWorker(999));
}

// ===========================================================================
// Part 12: AuxProcessMain integration
// ===========================================================================

TEST_F(AuxProcessTest, AuxProcessMainBgWriter) {
    // Schedule some flush work so BgWriterMain does something.
    mytoydb::server::BgWriterScheduleFlush(/*target_count=*/5);
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
