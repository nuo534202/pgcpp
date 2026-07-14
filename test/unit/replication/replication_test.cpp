// replication_test.cpp — Unit tests for the pgcpp replication subsystem
// (Task 15.20.4). Covers both physical (streaming) and logical replication:
//
//   - WalSnd:        init, set/get state, multiple senders, alloc/remove
//   - WalRcv:        start / stop / state / LSN reporting
//   - WalSnd msgs:   WalSndWriteData / keepalive / reply
//   - ReplicationSlot: create / acquire / release / drop / persist /
//                     advance / lookup by name
//   - slotfuncs:     pg_create / pg_drop / pg_replication_slot_advance
//   - Logical decoding: CreateInit / CreateDecoding / LogicalShippingMain
//   - Worker pool:    add / remove / find by subid / ApplyWorkerMain
//   - Repl origin:   create / drop / advance / session set/get / lookup
//   - Launcher:       init / main / wakeup / shutdown / id allocation
//   - SyncRep:        config update / parse / lookup / wait-for-LSN
//   - Backup:         start / do / stop
//   - replutil:       msg-type table / wal_level accessor
//
// The fixture mirrors the AllocSetContext pattern used elsewhere in the
// project: each test runs with a fresh memory context as CurrentMemoryContext
// and the error subsystem initialized. PG_TRY is used around calls that
// may ereport(ERROR) so gtest doesn't get longjmp'd past.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "replication/backup.hpp"
#include "replication/launcher.hpp"
#include "replication/logical.hpp"
#include "replication/logicalproto.hpp"
#include "replication/origin.hpp"
#include "replication/replutil.hpp"
#include "replication/slot.hpp"
#include "replication/slotfuncs.hpp"
#include "replication/syncrep.hpp"
#include "replication/walreceiver.hpp"
#include "replication/walsender.hpp"
#include "replication/walsenderfuncs.hpp"
#include "replication/worker.hpp"
#include "transaction/xlog.hpp"

using pgcpp::replication::AllocateNextSubscriptionId;
using pgcpp::replication::ApplyLauncherInit;
using pgcpp::replication::ApplyLauncherIsRunning;
using pgcpp::replication::ApplyLauncherMain;
using pgcpp::replication::ApplyLauncherReset;
using pgcpp::replication::ApplyLauncherShutdown;
using pgcpp::replication::ApplyLauncherWakeup;
using pgcpp::replication::ApplyStandbyReply;
using pgcpp::replication::ApplyWorkerMain;
using pgcpp::replication::ApplyWorkerWakeup;
using pgcpp::replication::BackupHandle;
using pgcpp::replication::BackupState;
using pgcpp::replication::BackupStateName;
using pgcpp::replication::CommitOrigin;
using pgcpp::replication::CreateDecodingContext;
using pgcpp::replication::CreateInitDecodingContext;
using pgcpp::replication::DecodingEmitMessage;
using pgcpp::replication::DefaultOutputPluginName;
using pgcpp::replication::DoBackup;
using pgcpp::replication::GetCurrentBackup;
using pgcpp::replication::GetLogicalDecodingContext;
using pgcpp::replication::GetLogicalRepLauncherState;
using pgcpp::replication::GetLogicalRepWorkerPool;
using pgcpp::replication::GetReplicationSlotCtl;
using pgcpp::replication::GetWalLevel;
using pgcpp::replication::GetWalRcvData;
using pgcpp::replication::GetWalSndCtl;
using pgcpp::replication::GetWalSndStats;
using pgcpp::replication::InitializeBackup;
using pgcpp::replication::kFirstUserRepOriginId;
using pgcpp::replication::kInvalidRepOriginId;
using pgcpp::replication::kMaxWalSenders;
using pgcpp::replication::LogicalDecodingContext;
using pgcpp::replication::LogicalDecodingOptions;
using pgcpp::replication::LogicalDecodingReset;
using pgcpp::replication::LogicalRepMsgType;
using pgcpp::replication::LogicalRepMsgTypeName;
using pgcpp::replication::LogicalRepWorker;
using pgcpp::replication::LogicalRepWorkerAdd;
using pgcpp::replication::LogicalRepWorkerCount;
using pgcpp::replication::LogicalRepWorkerFindBySub;
using pgcpp::replication::LogicalRepWorkerGetByIndex;
using pgcpp::replication::LogicalRepWorkerInit;
using pgcpp::replication::LogicalRepWorkerRemove;
using pgcpp::replication::LogicalRepWorkerReset;
using pgcpp::replication::LogicalRepWorkerType;
using pgcpp::replication::LogicalShippingMain;
using pgcpp::replication::LogLogicalMessage;
using pgcpp::replication::ParseLogicalMessage;
using pgcpp::replication::PgCreateReplicationSlot;
using pgcpp::replication::PgCreateReplicationSlotResult;
using pgcpp::replication::PgDropReplicationSlot;
using pgcpp::replication::PgLogicalEmitMessage;
using pgcpp::replication::PgLogicalSlotGetChanges;
using pgcpp::replication::PgLogicalSlotPeekChanges;
using pgcpp::replication::PgReplicationSlotAdvance;
using pgcpp::replication::PgReplicationSlotAdvanceToCurrent;
using pgcpp::replication::ReplicationSlot;
using pgcpp::replication::ReplicationSlotAcquire;
using pgcpp::replication::ReplicationSlotAdvance;
using pgcpp::replication::ReplicationSlotCount;
using pgcpp::replication::ReplicationSlotCreate;
using pgcpp::replication::ReplicationSlotDrop;
using pgcpp::replication::ReplicationSlotInit;
using pgcpp::replication::ReplicationSlotLookup;
using pgcpp::replication::ReplicationSlotPersist;
using pgcpp::replication::ReplicationSlotRelease;
using pgcpp::replication::ReploriginAdvance;
using pgcpp::replication::ReploriginCount;
using pgcpp::replication::ReploriginCreate;
using pgcpp::replication::ReploriginDrop;
using pgcpp::replication::ReploriginDropByName;
using pgcpp::replication::ReploriginGet;
using pgcpp::replication::ReploriginGetByName;
using pgcpp::replication::ReplOriginInit;
using pgcpp::replication::ReplOriginReset;
using pgcpp::replication::ReploriginSessionGet;
using pgcpp::replication::ReploriginSessionLsn;
using pgcpp::replication::ReploriginSessionReset;
using pgcpp::replication::ReploriginSessionSet;
using pgcpp::replication::RepOriginId;
using pgcpp::replication::SerializeLogicalMessage;
using pgcpp::replication::SlotPersistence;
using pgcpp::replication::SlotPersistenceName;
using pgcpp::replication::SlotType;
using pgcpp::replication::SlotTypeName;
using pgcpp::replication::StartBackup;
using pgcpp::replication::StopBackup;
using pgcpp::replication::SyncRepConfig;
using pgcpp::replication::SyncRepConfigGet;
using pgcpp::replication::SyncRepConfigInit;
using pgcpp::replication::SyncRepConfigParse;
using pgcpp::replication::SyncRepConfigReset;
using pgcpp::replication::SyncRepConfigUpdate;
using pgcpp::replication::SyncRepGetWaiters;
using pgcpp::replication::SyncRepIsSyncStandby;
using pgcpp::replication::SyncRepSyncMethod;
using pgcpp::replication::SyncRepWaitForLSN;
using pgcpp::replication::WalLevel;
using pgcpp::replication::WalRcvData;
using pgcpp::replication::WalRcvGetState;
using pgcpp::replication::WalRcvGetStreamState;
using pgcpp::replication::WalRcvInit;
using pgcpp::replication::WalRcvLsnKind;
using pgcpp::replication::WalRcvReportLsn;
using pgcpp::replication::WalRcvSetPid;
using pgcpp::replication::WalRcvStart;
using pgcpp::replication::WalRcvState;
using pgcpp::replication::WalRcvStateName;
using pgcpp::replication::WalRcvStop;
using pgcpp::replication::WalSnd;
using pgcpp::replication::WalSndAlloc;
using pgcpp::replication::WalSndCount;
using pgcpp::replication::WalSndCtlData;
using pgcpp::replication::WalSndGetByIndex;
using pgcpp::replication::WalSndGetByPid;
using pgcpp::replication::WalSndGetState;
using pgcpp::replication::WalSndInit;
using pgcpp::replication::WalSndKeepalive;
using pgcpp::replication::WalSndLsnKind;
using pgcpp::replication::WalSndMessageResult;
using pgcpp::replication::WalSndRemove;
using pgcpp::replication::WalSndReplyMessage;
using pgcpp::replication::WalSndSetState;
using pgcpp::replication::WalSndState;
using pgcpp::replication::WalSndStats;
using pgcpp::replication::WalSndUpdateLsn;
using pgcpp::replication::WalSndWaitForWal;
using pgcpp::replication::WalSndWakeup;
using pgcpp::replication::WalSndWriteData;
using pgcpp::replication::xl_logical_message;
using pgcpp::transaction::GetXLogInsertRecPtr;
using pgcpp::transaction::InitializeWal;
using pgcpp::transaction::ResetWal;
using pgcpp::transaction::XLogRecPtr;

namespace {

// ReplicationTest — AllocSetContext fixture used by all replication tests.
//
// Each test starts with a fresh memory context as CurrentMemoryContext,
// the error subsystem initialized (so ereport(ERROR) has a place to land
// its ErrorData), and every replication subsystem reset to its initial
// state. TearDown restores CurrentMemoryContext and frees the context.
class ReplicationTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("repl_test_ctx");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        ResetWal();
        InitializeWal();

        WalSndInit();
        WalRcvInit();
        ReplicationSlotInit();
        LogicalDecodingReset();
        LogicalRepWorkerInit();
        ReplOriginInit();
        ApplyLauncherInit();
        SyncRepConfigInit();
        InitializeBackup();
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    pgcpp::memory::MemoryContext* context_ = nullptr;
};

// Helper that runs `body` inside PG_TRY and returns true if no error
// was raised. The lambda body should NOT call FAIL() on error.
template<typename Body>
bool NoError(Body body) {
    bool ok = true;
    PG_TRY() {
        body();
    }
    PG_CATCH() {
        ok = false;
    }
    PG_END_TRY();
    return ok;
}

}  // namespace

// ===========================================================================
// Part 1: replutil — message type names and wal_level accessor
// ===========================================================================

TEST_F(ReplicationTest, LogicalRepMsgTypeNameCoversAllValues) {
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kBegin), "BEGIN");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kCommit), "COMMIT");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kInsert), "INSERT");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kUpdate), "UPDATE");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kDelete), "DELETE");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kTruncate), "TRUNCATE");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kRelation), "RELATION");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kType), "TYPE");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kOrigin), "ORIGIN");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kMessage), "MESSAGE");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kStreamStart), "STREAM START");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kStreamStop), "STREAM STOP");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kStreamCommit), "STREAM COMMIT");
    EXPECT_STREQ(LogicalRepMsgTypeName(LogicalRepMsgType::kStreamAbort), "STREAM ABORT");
}

TEST_F(ReplicationTest, WalLevelDefaultsToReplica) {
    EXPECT_EQ(GetWalLevel(), WalLevel::kReplica);
    pgcpp::replication::SetWalLevel(WalLevel::kLogical);
    EXPECT_EQ(GetWalLevel(), WalLevel::kLogical);
    pgcpp::replication::SetWalLevel(WalLevel::kReplica);
    EXPECT_EQ(GetWalLevel(), WalLevel::kReplica);
}

TEST_F(ReplicationTest, DefaultOutputPluginIsPgoutput) {
    EXPECT_STREQ(DefaultOutputPluginName(), "pgoutput");
}

// ===========================================================================
// Part 2: WalSnd — init, set/get state, multiple senders
// ===========================================================================

TEST_F(ReplicationTest, WalSndInitClearsArray) {
    EXPECT_EQ(WalSndCount(), 0);
    EXPECT_EQ(GetWalSndCtl()->walsenders.size(), 0u);
    EXPECT_EQ(GetWalSndCtl()->lsn_target, 0u);
}

TEST_F(ReplicationTest, WalSndAllocAddsSender) {
    int idx = WalSndAlloc(/*pid=*/100, "standby_a");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(WalSndCount(), 1);
    WalSnd* s = WalSndGetByIndex(idx);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->pid, 100);
    EXPECT_EQ(s->application_name, "standby_a");
    EXPECT_EQ(s->state, WalSndState::kStartup);
}

TEST_F(ReplicationTest, WalSndGetStateInvalidIndexReturnsStopping) {
    EXPECT_EQ(WalSndGetState(0), WalSndState::kStopping);
    EXPECT_EQ(WalSndGetState(-1), WalSndState::kStopping);
}

TEST_F(ReplicationTest, WalSndSetStateTransitions) {
    int idx = WalSndAlloc(101, "standby_b");
    ASSERT_GE(idx, 0);
    EXPECT_TRUE(WalSndSetState(idx, WalSndState::kCatchup));
    EXPECT_EQ(WalSndGetState(idx), WalSndState::kCatchup);
    EXPECT_TRUE(WalSndSetState(idx, WalSndState::kStreaming));
    EXPECT_EQ(WalSndGetState(idx), WalSndState::kStreaming);
    EXPECT_FALSE(WalSndSetState(-1, WalSndState::kStreaming));
}

TEST_F(ReplicationTest, WalSndMultipleSenders) {
    int i1 = WalSndAlloc(1, "s1");
    int i2 = WalSndAlloc(2, "s2");
    int i3 = WalSndAlloc(3, "s3");
    ASSERT_GE(i1, 0);
    ASSERT_GE(i2, 0);
    ASSERT_GE(i3, 0);
    EXPECT_EQ(WalSndCount(), 3);
    EXPECT_EQ(WalSndGetByPid(1), i1);
    EXPECT_EQ(WalSndGetByPid(2), i2);
    EXPECT_EQ(WalSndGetByPid(3), i3);
    EXPECT_EQ(WalSndGetByPid(999), -1);
}

TEST_F(ReplicationTest, WalSndGetByIndexInvalidReturnsNull) {
    EXPECT_EQ(WalSndGetByIndex(-1), nullptr);
    EXPECT_EQ(WalSndGetByIndex(0), nullptr);
}

TEST_F(ReplicationTest, WalSndRemoveShiftsLaterSenders) {
    int i1 = WalSndAlloc(10, "a");
    int i2 = WalSndAlloc(20, "b");
    int i3 = WalSndAlloc(30, "c");
    ASSERT_EQ(i1, 0);
    ASSERT_EQ(i2, 1);
    ASSERT_EQ(i3, 2);

    EXPECT_TRUE(WalSndRemove(i1));
    EXPECT_EQ(WalSndCount(), 2);
    // After erase, index 0 holds what was at index 1.
    WalSnd* s = WalSndGetByIndex(0);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->pid, 20);
    EXPECT_FALSE(WalSndRemove(-1));
    EXPECT_FALSE(WalSndRemove(99));
}

TEST_F(ReplicationTest, WalSndUpdateLsn) {
    int idx = WalSndAlloc(42, "lsn_tester");
    ASSERT_GE(idx, 0);
    WalSndUpdateLsn(idx, WalSndLsnKind::kSent, 100);
    WalSndUpdateLsn(idx, WalSndLsnKind::kWrite, 90);
    WalSndUpdateLsn(idx, WalSndLsnKind::kFlush, 80);
    WalSndUpdateLsn(idx, WalSndLsnKind::kApply, 70);
    WalSnd* s = WalSndGetByIndex(idx);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->sent_ptr, 100u);
    EXPECT_EQ(s->write_ptr, 90u);
    EXPECT_EQ(s->flush_ptr, 80u);
    EXPECT_EQ(s->apply_ptr, 70u);
}

TEST_F(ReplicationTest, WalSndWakeupMarksAllForFlush) {
    WalSndAlloc(1, "a");
    WalSndAlloc(2, "b");
    WalSndWakeup();
    WalSnd* s0 = WalSndGetByIndex(0);
    WalSnd* s1 = WalSndGetByIndex(1);
    ASSERT_NE(s0, nullptr);
    ASSERT_NE(s1, nullptr);
    EXPECT_TRUE(s0->need_to_flush);
    EXPECT_TRUE(s1->need_to_flush);
}

TEST_F(ReplicationTest, WalSndWaitForWalReturnsTrueWhenReachable) {
    int idx = WalSndAlloc(1, "w");
    ASSERT_GE(idx, 0);
    XLogRecPtr cur = GetXLogInsertRecPtr();
    EXPECT_TRUE(WalSndWaitForWal(cur, -1, 0));
    EXPECT_FALSE(WalSndWaitForWal(cur + 1, idx, 0));
}

TEST_F(ReplicationTest, WalSndAllocFailsAtCapacity) {
    for (int i = 0; i < kMaxWalSenders; ++i) {
        ASSERT_GE(WalSndAlloc(1000 + i, "s" + std::to_string(i)), 0);
    }
    EXPECT_FALSE(NoError([&] { (void)WalSndAlloc(9999, "overflow"); }));
}

// ===========================================================================
// Part 3: WalSnd message helpers (walsenderfuncs)
// ===========================================================================

TEST_F(ReplicationTest, WalSndWriteDataUpdatesSentPtrAndBytes) {
    int idx = WalSndAlloc(7, "writer");
    ASSERT_GE(idx, 0);
    WalSndMessageResult r = WalSndWriteData(idx, /*start=*/100, /*end=*/200, true);
    EXPECT_EQ(r.start_lsn, 100u);
    EXPECT_EQ(r.end_lsn, 200u);
    EXPECT_EQ(r.bytes_sent, 100u);
    EXPECT_TRUE(r.reply_requested);
    WalSnd* s = WalSndGetByIndex(idx);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->sent_ptr, 200u);
    EXPECT_TRUE(s->need_to_flush);
}

TEST_F(ReplicationTest, WalSndWriteDataInvalidIndexErrors) {
    EXPECT_FALSE(NoError([&] { WalSndWriteData(-1, 0, 10, false); }));
}

TEST_F(ReplicationTest, WalSndWriteDataRejectsInvertedRange) {
    int idx = WalSndAlloc(7, "inverted");
    ASSERT_GE(idx, 0);
    EXPECT_FALSE(NoError([&] { WalSndWriteData(idx, 200, 100, false); }));
}

TEST_F(ReplicationTest, WalSndKeepaliveAdvancesSentPtrToCurrentInsert) {
    int idx = WalSndAlloc(7, "ka");
    ASSERT_GE(idx, 0);
    XLogRecPtr cur = GetXLogInsertRecPtr();
    WalSndMessageResult r = WalSndKeepalive(idx, /*reply_requested=*/true);
    EXPECT_EQ(r.end_lsn, cur);
    WalSnd* s = WalSndGetByIndex(idx);
    ASSERT_NE(s, nullptr);
    EXPECT_GE(s->sent_ptr, cur);
    EXPECT_TRUE(s->need_to_flush);
}

TEST_F(ReplicationTest, ApplyStandbyReplyAdvancesLsns) {
    int idx = WalSndAlloc(7, "replier");
    ASSERT_GE(idx, 0);
    WalSndReplyMessage reply;
    reply.write_lsn = 100;
    reply.flush_lsn = 90;
    reply.apply_lsn = 80;
    reply.reply_requested = false;
    EXPECT_EQ(ApplyStandbyReply(idx, reply), idx);
    WalSnd* s = WalSndGetByIndex(idx);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->write_ptr, 100u);
    EXPECT_EQ(s->flush_ptr, 90u);
    EXPECT_EQ(s->apply_ptr, 80u);
    EXPECT_FALSE(s->need_to_flush);
}

TEST_F(ReplicationTest, ApplyStandbyReplyInvalidIndexReturnsNeg1) {
    WalSndReplyMessage reply;
    EXPECT_EQ(ApplyStandbyReply(-1, reply), -1);
}

TEST_F(ReplicationTest, GetWalSndStatsCountsActiveAndStreaming) {
    WalSndAlloc(1, "a");
    int idx2 = WalSndAlloc(2, "b");
    WalSndAlloc(3, "c");
    WalSndSetState(idx2, WalSndState::kStreaming);
    WalSndStats stats = GetWalSndStats();
    EXPECT_EQ(stats.active_senders, 3);
    EXPECT_EQ(stats.streaming_senders, 1);
}

// ===========================================================================
// Part 4: WalRcv — start / stop / state / LSN reporting
// ===========================================================================

TEST_F(ReplicationTest, WalRcvInitialStateStopped) {
    EXPECT_EQ(WalRcvGetState(), WalRcvState::kStopped);
    EXPECT_EQ(WalRcvGetStreamState(), pgcpp::replication::WalRcvStreamState::kNone);
    EXPECT_STREQ(WalRcvStateName(WalRcvState::kStopped), "stopped");
    EXPECT_STREQ(WalRcvStateName(WalRcvState::kStreaming), "streaming");
}

TEST_F(ReplicationTest, WalRcvStartTransitionsToStreaming) {
    EXPECT_TRUE(WalRcvStart("host=primary port=5432", "slot1", /*startpoint=*/1234));
    EXPECT_EQ(WalRcvGetState(), WalRcvState::kStreaming);
    WalRcvData* d = GetWalRcvData();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->conninfo, "host=primary port=5432");
    EXPECT_EQ(d->slotname, "slot1");
    EXPECT_EQ(d->startpoint, 1234u);
    EXPECT_EQ(d->receive_ptr, 1234u);
}

TEST_F(ReplicationTest, WalRcvStartRejectsEmptyConninfo) {
    EXPECT_FALSE(NoError([&] { WalRcvStart("", "", 0); }));
}

TEST_F(ReplicationTest, WalRcvStartRejectsDoubleStart) {
    ASSERT_TRUE(WalRcvStart("host=p", "s", 0));
    EXPECT_FALSE(NoError([&] { WalRcvStart("host=p2", "s2", 0); }));
}

TEST_F(ReplicationTest, WalRcvStopReturnsPidAndClearsState) {
    WalRcvSetPid(4321);
    ASSERT_TRUE(WalRcvStart("host=p", "s", 0));
    int32_t pid = WalRcvStop();
    EXPECT_EQ(pid, 4321);
    EXPECT_EQ(WalRcvGetState(), WalRcvState::kStopped);
    EXPECT_EQ(WalRcvStop(), 0);  // already stopped
}

TEST_F(ReplicationTest, WalRcvReportLsnAdvances) {
    ASSERT_TRUE(WalRcvStart("host=p", "s", 1000));
    WalRcvReportLsn(WalRcvLsnKind::kReceive, 1500);
    WalRcvReportLsn(WalRcvLsnKind::kWrite, 1400);
    WalRcvReportLsn(WalRcvLsnKind::kFlush, 1300);
    WalRcvReportLsn(WalRcvLsnKind::kApply, 1200);
    WalRcvData* d = GetWalRcvData();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->receive_ptr, 1500u);
    EXPECT_EQ(d->write_ptr, 1400u);
    EXPECT_EQ(d->flush_ptr, 1300u);
    EXPECT_EQ(d->apply_ptr, 1200u);
}

// ===========================================================================
// Part 5: ReplicationSlot — create / acquire / release / drop / persist
// ===========================================================================

TEST_F(ReplicationTest, ReplicationSlotInitClearsStore) {
    EXPECT_EQ(ReplicationSlotCount(), 0);
    EXPECT_EQ(GetReplicationSlotCtl()->slots.size(), 0u);
}

TEST_F(ReplicationTest, ReplicationSlotCreatePhysical) {
    EXPECT_TRUE(ReplicationSlotCreate("p1", SlotType::kPhysical, SlotPersistence::kPersistent,
                                      /*plugin=*/"", /*database=*/""));
    EXPECT_EQ(ReplicationSlotCount(), 1);
    const ReplicationSlot* s = ReplicationSlotLookup("p1");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->name, "p1");
    EXPECT_EQ(s->type, SlotType::kPhysical);
    EXPECT_EQ(s->persistence, SlotPersistence::kPersistent);
    EXPECT_FALSE(s->active);
    EXPECT_TRUE(s->dirty);
}

TEST_F(ReplicationTest, ReplicationSlotCreateLogical) {
    EXPECT_TRUE(ReplicationSlotCreate("l1", SlotType::kLogical, SlotPersistence::kPersistent,
                                      "pgoutput", "postgres"));
    const ReplicationSlot* s = ReplicationSlotLookup("l1");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->type, SlotType::kLogical);
    EXPECT_EQ(s->plugin, "pgoutput");
    EXPECT_EQ(s->database, "postgres");
}

TEST_F(ReplicationTest, ReplicationSlotCreateRejectsEmptyName) {
    EXPECT_FALSE(NoError([&] {
        ReplicationSlotCreate("", SlotType::kPhysical, SlotPersistence::kPersistent, "", "");
    }));
}

TEST_F(ReplicationTest, ReplicationSlotCreateRejectsDuplicateName) {
    ASSERT_TRUE(
        ReplicationSlotCreate("dup", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    EXPECT_FALSE(NoError([&] {
        ReplicationSlotCreate("dup", SlotType::kPhysical, SlotPersistence::kPersistent, "", "");
    }));
    EXPECT_EQ(ReplicationSlotCount(), 1);
}

TEST_F(ReplicationTest, ReplicationSlotCreateLogicalRequiresPlugin) {
    EXPECT_FALSE(NoError([&] {
        ReplicationSlotCreate("no_plugin", SlotType::kLogical, SlotPersistence::kPersistent, "",
                              "postgres");
    }));
}

TEST_F(ReplicationTest, ReplicationSlotAcquireMarksActive) {
    ASSERT_TRUE(
        ReplicationSlotCreate("acq", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    EXPECT_TRUE(ReplicationSlotAcquire("acq", /*pid=*/42));
    const ReplicationSlot* s = ReplicationSlotLookup("acq");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->active);
    EXPECT_EQ(s->active_pid, 42);
    EXPECT_TRUE(s->just_started);
}

TEST_F(ReplicationTest, ReplicationSlotAcquireRejectsMissing) {
    EXPECT_FALSE(NoError([&] { ReplicationSlotAcquire("missing", 1); }));
}

TEST_F(ReplicationTest, ReplicationSlotAcquireRejectsDoubleAcquire) {
    ASSERT_TRUE(
        ReplicationSlotCreate("acq2", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    ASSERT_TRUE(ReplicationSlotAcquire("acq2", 7));
    EXPECT_FALSE(NoError([&] { ReplicationSlotAcquire("acq2", 8); }));
}

TEST_F(ReplicationTest, ReplicationSlotReleaseByPid) {
    ASSERT_TRUE(
        ReplicationSlotCreate("rel", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    ASSERT_TRUE(ReplicationSlotAcquire("rel", 77));
    EXPECT_EQ(ReplicationSlotRelease(77), "rel");
    const ReplicationSlot* s = ReplicationSlotLookup("rel");
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->active);
    EXPECT_EQ(s->active_pid, 0);
    // Releasing an unknown pid returns empty string.
    EXPECT_EQ(ReplicationSlotRelease(9999), "");
}

TEST_F(ReplicationTest, ReplicationSlotDrop) {
    ASSERT_TRUE(
        ReplicationSlotCreate("dropme", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    EXPECT_TRUE(ReplicationSlotDrop("dropme"));
    EXPECT_EQ(ReplicationSlotLookup("dropme"), nullptr);
    EXPECT_EQ(ReplicationSlotCount(), 0);
}

TEST_F(ReplicationTest, ReplicationSlotDropMissingErrors) {
    EXPECT_FALSE(NoError([&] { ReplicationSlotDrop("nonexistent"); }));
}

TEST_F(ReplicationTest, ReplicationSlotDropActiveErrors) {
    ASSERT_TRUE(ReplicationSlotCreate("active_drop", SlotType::kPhysical,
                                      SlotPersistence::kPersistent, "", ""));
    ASSERT_TRUE(ReplicationSlotAcquire("active_drop", 1));
    EXPECT_FALSE(NoError([&] { ReplicationSlotDrop("active_drop"); }));
}

TEST_F(ReplicationTest, ReplicationSlotPersistUpgradesTemporary) {
    ASSERT_TRUE(
        ReplicationSlotCreate("tmp", SlotType::kPhysical, SlotPersistence::kTemporary, "", ""));
    EXPECT_TRUE(ReplicationSlotPersist("tmp"));
    const ReplicationSlot* s = ReplicationSlotLookup("tmp");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->persistence, SlotPersistence::kPersistent);
    EXPECT_TRUE(s->dirty);
}

TEST_F(ReplicationTest, ReplicationSlotAdvancePhysical) {
    ASSERT_TRUE(
        ReplicationSlotCreate("adv_p", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    const ReplicationSlot* s = ReplicationSlotLookup("adv_p");
    ASSERT_NE(s, nullptr);
    XLogRecPtr initial = s->restart_lsn;
    EXPECT_EQ(ReplicationSlotAdvance("adv_p", initial + 500), initial + 500);
    EXPECT_EQ(s->restart_lsn, initial + 500);
    // Advancing backward is a no-op.
    EXPECT_EQ(ReplicationSlotAdvance("adv_p", initial), initial + 500);
}

TEST_F(ReplicationTest, ReplicationSlotAdvanceLogical) {
    ASSERT_TRUE(ReplicationSlotCreate("adv_l", SlotType::kLogical, SlotPersistence::kPersistent,
                                      "pgoutput", "postgres"));
    const ReplicationSlot* s = ReplicationSlotLookup("adv_l");
    ASSERT_NE(s, nullptr);
    XLogRecPtr initial = s->confirmed_flush_lsn;
    EXPECT_EQ(ReplicationSlotAdvance("adv_l", initial + 1000), initial + 1000);
    EXPECT_EQ(s->confirmed_flush_lsn, initial + 1000);
}

TEST_F(ReplicationTest, ReplicationSlotLookupMissingReturnsNull) {
    EXPECT_EQ(ReplicationSlotLookup("does_not_exist"), nullptr);
}

TEST_F(ReplicationTest, SlotTypeNames) {
    EXPECT_STREQ(SlotTypeName(SlotType::kPhysical), "physical");
    EXPECT_STREQ(SlotTypeName(SlotType::kLogical), "logical");
    EXPECT_STREQ(SlotPersistenceName(SlotPersistence::kPersistent), "persistent");
    EXPECT_STREQ(SlotPersistenceName(SlotPersistence::kTemporary), "temporary");
}

// ===========================================================================
// Part 6: slotfuncs — pg_create / pg_drop / pg_replication_slot_advance
// ===========================================================================

TEST_F(ReplicationTest, PgCreateReplicationSlotPhysical) {
    PgCreateReplicationSlotResult r =
        PgCreateReplicationSlot("pg_p", /*plugin=*/"", /*is_logical=*/false, "");
    EXPECT_EQ(r.slot_name, "pg_p");
    EXPECT_GT(r.start_lsn, 0u);
    EXPECT_TRUE(r.snapshot_name.empty());
    EXPECT_NE(ReplicationSlotLookup("pg_p"), nullptr);
}

TEST_F(ReplicationTest, PgCreateReplicationSlotLogicalAssignsSnapshot) {
    PgCreateReplicationSlotResult r =
        PgCreateReplicationSlot("pg_l", /*plugin=*/"", /*is_logical=*/true, "postgres");
    EXPECT_EQ(r.slot_name, "pg_l");
    EXPECT_GT(r.start_lsn, 0u);
    EXPECT_FALSE(r.snapshot_name.empty());
    EXPECT_NE(r.snapshot_name.find("pg_l"), std::string::npos);
    const ReplicationSlot* s = ReplicationSlotLookup("pg_l");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->plugin, "pgoutput");  // defaulted
}

TEST_F(ReplicationTest, PgCreateReplicationSlotDuplicateReturnsZeroLsn) {
    ASSERT_TRUE(PgCreateReplicationSlot("pg_dup", "", false, "").start_lsn > 0);
    auto r = PgCreateReplicationSlot("pg_dup", "", false, "");
    EXPECT_EQ(r.start_lsn, 0u);  // failure path
    EXPECT_EQ(ReplicationSlotCount(), 1);
}

TEST_F(ReplicationTest, PgDropReplicationSlot) {
    ASSERT_TRUE(PgCreateReplicationSlot("pg_drop", "", false, "").start_lsn > 0);
    EXPECT_TRUE(PgDropReplicationSlot("pg_drop"));
    EXPECT_EQ(ReplicationSlotLookup("pg_drop"), nullptr);
    EXPECT_FALSE(PgDropReplicationSlot("pg_drop"));  // already gone
}

TEST_F(ReplicationTest, PgReplicationSlotAdvance) {
    ASSERT_TRUE(PgCreateReplicationSlot("pg_adv", "", false, "").start_lsn > 0);
    const ReplicationSlot* s = ReplicationSlotLookup("pg_adv");
    ASSERT_NE(s, nullptr);
    XLogRecPtr initial = s->restart_lsn;
    XLogRecPtr new_lsn = PgReplicationSlotAdvance("pg_adv", initial + 50);
    EXPECT_EQ(new_lsn, initial + 50);
}

TEST_F(ReplicationTest, PgReplicationSlotAdvanceToCurrent) {
    ASSERT_TRUE(PgCreateReplicationSlot("pg_adv2", "", false, "").start_lsn > 0);
    XLogRecPtr cur = GetXLogInsertRecPtr();
    XLogRecPtr new_lsn = PgReplicationSlotAdvanceToCurrent("pg_adv2");
    EXPECT_EQ(new_lsn, cur);
}

TEST_F(ReplicationTest, PgReplicationSlotAdvanceMissingReturnsZero) {
    EXPECT_EQ(PgReplicationSlotAdvance("nonexistent", 1000), 0u);
}

// ===========================================================================
// Part 7: LogicalDecodingContext — CreateInit / CreateDecoding / ShippingMain
// ===========================================================================

TEST_F(ReplicationTest, CreateInitDecodingContextBuildsSlot) {
    LogicalDecodingOptions opts;
    opts.streaming = true;
    LogicalDecodingContext ctx =
        CreateInitDecodingContext("pgoutput", "ldc_init", "postgres", opts);
    EXPECT_TRUE(ctx.prepared);
    EXPECT_EQ(ctx.plugin_name, "pgoutput");
    EXPECT_EQ(ctx.slot_name, "ldc_init");
    EXPECT_GT(ctx.start_lsn, 0u);
    const ReplicationSlot* s = ReplicationSlotLookup("ldc_init");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->type, SlotType::kLogical);
}

TEST_F(ReplicationTest, CreateInitDecodingContextDefaultsPluginName) {
    LogicalDecodingContext ctx =
        CreateInitDecodingContext("", "ldc_def", "postgres", LogicalDecodingOptions{});
    EXPECT_EQ(ctx.plugin_name, "pgoutput");
}

TEST_F(ReplicationTest, CreateInitDecodingContextRejectsEmptySlotName) {
    EXPECT_FALSE(NoError(
        [&] { CreateInitDecodingContext("pgoutput", "", "postgres", LogicalDecodingOptions{}); }));
}

TEST_F(ReplicationTest, CreateDecodingContextOpensExistingSlot) {
    ASSERT_TRUE(ReplicationSlotCreate("ldc_open", SlotType::kLogical, SlotPersistence::kPersistent,
                                      "pgoutput", "postgres"));
    LogicalDecodingContext ctx = CreateDecodingContext("ldc_open", LogicalDecodingOptions{});
    EXPECT_TRUE(ctx.prepared);
    EXPECT_EQ(ctx.slot_name, "ldc_open");
    EXPECT_EQ(ctx.plugin_name, "pgoutput");
}

TEST_F(ReplicationTest, CreateDecodingContextRejectsMissingSlot) {
    EXPECT_FALSE(NoError([&] { CreateDecodingContext("missing_slot", LogicalDecodingOptions{}); }));
}

TEST_F(ReplicationTest, CreateDecodingContextRejectsPhysicalSlot) {
    ASSERT_TRUE(
        ReplicationSlotCreate("phys", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    EXPECT_FALSE(NoError([&] { CreateDecodingContext("phys", LogicalDecodingOptions{}); }));
}

TEST_F(ReplicationTest, LogicalShippingMainDecodesEmittedMessages) {
    // Create a logical slot. Its restart_lsn is the current WAL insert ptr.
    LogicalDecodingContext ctx =
        CreateInitDecodingContext("pgoutput", "ship", "postgres", LogicalDecodingOptions{});
    ASSERT_TRUE(ctx.prepared);

    // Emit two logical messages to the WAL after the slot was created.
    // These land at LSN > slot.restart_lsn and will be picked up by the
    // decoding loop.
    XLogRecPtr lsn1 = LogLogicalMessage(/*transactional=*/false, "p1", "hello");
    XLogRecPtr lsn2 = LogLogicalMessage(/*transactional=*/false, "p2", "world");
    ASSERT_GT(lsn1, 0u);
    ASSERT_GT(lsn2, lsn1);

    int emitted = LogicalShippingMain(ctx, /*max_messages=*/0);
    EXPECT_EQ(emitted, 2);
    ASSERT_EQ(ctx.messages.size(), 2u);
    EXPECT_NE(ctx.messages[0].find("MESSAGE"), std::string::npos);
    EXPECT_NE(ctx.messages[0].find("p1"), std::string::npos);
    EXPECT_NE(ctx.messages[0].find("hello"), std::string::npos);
    EXPECT_NE(ctx.messages[1].find("p2"), std::string::npos);
    EXPECT_GT(ctx.end_lsn, lsn2);
}

TEST_F(ReplicationTest, LogicalShippingMainHonorsMaxMessages) {
    LogicalDecodingContext ctx =
        CreateInitDecodingContext("pgoutput", "ship_max", "postgres", LogicalDecodingOptions{});
    ASSERT_TRUE(ctx.prepared);

    LogLogicalMessage(false, "p", "m1");
    LogLogicalMessage(false, "p", "m2");
    LogLogicalMessage(false, "p", "m3");

    int emitted = LogicalShippingMain(ctx, /*max_messages=*/2);
    EXPECT_EQ(emitted, 2);
    EXPECT_EQ(ctx.messages.size(), 2u);
}

TEST_F(ReplicationTest, LogicalShippingMainRejectsUnpreparedContext) {
    LogicalDecodingContext ctx;  // not prepared
    EXPECT_FALSE(NoError([&] { LogicalShippingMain(ctx, 1); }));
}

TEST_F(ReplicationTest, DecodingEmitMessageAppendsToBuffer) {
    LogicalDecodingContext ctx;
    DecodingEmitMessage(ctx, LogicalRepMsgType::kInsert, "row1");
    DecodingEmitMessage(ctx, LogicalRepMsgType::kUpdate, "row2");
    ASSERT_EQ(ctx.messages.size(), 2u);
    EXPECT_NE(ctx.messages[0].find("INSERT"), std::string::npos);
    EXPECT_NE(ctx.messages[0].find("row1"), std::string::npos);
    EXPECT_NE(ctx.messages[1].find("UPDATE"), std::string::npos);
    EXPECT_FALSE(ctx.output_buffer.empty());
}

TEST_F(ReplicationTest, GetLogicalDecodingContextReturnsLastCreated) {
    CreateInitDecodingContext("pgoutput", "gldc", "postgres", LogicalDecodingOptions{});
    LogicalDecodingContext* p = GetLogicalDecodingContext();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->slot_name, "gldc");
}

TEST_F(ReplicationTest, LogicalDecodingResetClearsActiveContext) {
    CreateInitDecodingContext("pgoutput", "rdc", "postgres", LogicalDecodingOptions{});
    LogicalDecodingReset();
    LogicalDecodingContext* p = GetLogicalDecodingContext();
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(p->prepared);
    EXPECT_TRUE(p->slot_name.empty());
}

// ===========================================================================
// Part 7b: Logical message WAL records (logicalproto)
// ===========================================================================

TEST_F(ReplicationTest, SerializeParseRoundTripPreservesFields) {
    xl_logical_message in;
    in.db_id = 12345;
    in.transactional = true;
    in.prefix = "myplugin";
    in.message = "hello world";
    std::vector<uint8_t> buf = SerializeLogicalMessage(in);
    ASSERT_FALSE(buf.empty());

    xl_logical_message out;
    ASSERT_TRUE(ParseLogicalMessage(buf.data(), buf.size(), out));
    EXPECT_EQ(out.db_id, in.db_id);
    EXPECT_EQ(out.transactional, in.transactional);
    EXPECT_EQ(out.prefix, in.prefix);
    EXPECT_EQ(out.message, in.message);
}

TEST_F(ReplicationTest, SerializeLayoutMatchesSpec) {
    xl_logical_message msg;
    msg.db_id = 1;
    msg.transactional = false;
    msg.prefix = "p";
    msg.message = "m";
    std::vector<uint8_t> buf = SerializeLogicalMessage(msg);
    // Layout: db_id(4) + transactional(1) + prefix_size(4) + message_size(4)
    //         + prefix(1) + message(1) = 15
    ASSERT_EQ(buf.size(), 15u);
    EXPECT_EQ(buf[4], 0);  // transactional = false
}

TEST_F(ReplicationTest, ParseRejectsTruncatedHeader) {
    std::vector<uint8_t> too_short(10, 0);  // less than 4+1+4+4=13
    xl_logical_message out;
    EXPECT_FALSE(ParseLogicalMessage(too_short.data(), too_short.size(), out));
}

TEST_F(ReplicationTest, ParseRejectsTruncatedPayload) {
    xl_logical_message msg;
    msg.prefix = "longprefix";
    msg.message = "longmessage";
    std::vector<uint8_t> buf = SerializeLogicalMessage(msg);
    // Truncate the payload portion.
    buf.resize(buf.size() - 5);
    xl_logical_message out;
    EXPECT_FALSE(ParseLogicalMessage(buf.data(), buf.size(), out));
}

TEST_F(ReplicationTest, ParseHandlesNullData) {
    xl_logical_message out;
    EXPECT_FALSE(ParseLogicalMessage(nullptr, 100, out));
}

TEST_F(ReplicationTest, ParseHandlesEmptyPrefixAndMessage) {
    xl_logical_message in;
    in.prefix = "";
    in.message = "";
    std::vector<uint8_t> buf = SerializeLogicalMessage(in);
    xl_logical_message out;
    ASSERT_TRUE(ParseLogicalMessage(buf.data(), buf.size(), out));
    EXPECT_TRUE(out.prefix.empty());
    EXPECT_TRUE(out.message.empty());
}

TEST_F(ReplicationTest, LogLogicalMessageWritesWalAndReturnsLsn) {
    XLogRecPtr before = GetXLogInsertRecPtr();
    XLogRecPtr lsn = LogLogicalMessage(false, "test", "payload");
    XLogRecPtr after = GetXLogInsertRecPtr();
    EXPECT_GT(lsn, 0u);
    EXPECT_GE(lsn, before);
    EXPECT_LT(lsn, after);  // WAL insert pointer advanced
}

TEST_F(ReplicationTest, LogLogicalMessageTransactionalFlag) {
    XLogRecPtr lsn = LogLogicalMessage(/*transactional=*/true, "txn", "data");
    EXPECT_GT(lsn, 0u);
}

// ===========================================================================
// Part 7c: SQL-facing logical decoding functions
// ===========================================================================

TEST_F(ReplicationTest, PgLogicalEmitMessageReturnsLsn) {
    XLogRecPtr lsn = PgLogicalEmitMessage(false, "p", "hello");
    EXPECT_GT(lsn, 0u);
}

TEST_F(ReplicationTest, PgLogicalSlotGetChangesRequiresExistingSlot) {
    EXPECT_FALSE(
        NoError([&] { (void)PgLogicalSlotGetChanges("nonexistent", /*upto_lsn=*/0, /*max=*/0); }));
}

TEST_F(ReplicationTest, PgLogicalSlotGetChangesRequiresLogicalSlot) {
    ASSERT_TRUE(
        ReplicationSlotCreate("phys", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    EXPECT_FALSE(
        NoError([&] { (void)PgLogicalSlotGetChanges("phys", /*upto_lsn=*/0, /*max=*/0); }));
}

TEST_F(ReplicationTest, PgLogicalSlotGetChangesDecodesAndAdvances) {
    // Create a logical slot — its restart_lsn = current WAL insert ptr.
    ASSERT_TRUE(ReplicationSlotCreate("log_slot", SlotType::kLogical, SlotPersistence::kPersistent,
                                      "pgoutput", "postgres"));
    const ReplicationSlot* s_before = ReplicationSlotLookup("log_slot");
    ASSERT_NE(s_before, nullptr);
    XLogRecPtr slot_lsn_before = s_before->confirmed_flush_lsn;

    // Emit two logical messages after the slot was created.
    PgLogicalEmitMessage(false, "p", "m1");
    PgLogicalEmitMessage(false, "p", "m2");

    // Get changes — should decode both messages.
    std::vector<std::string> changes =
        PgLogicalSlotGetChanges("log_slot", /*upto_lsn=*/0, /*max=*/0);
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_NE(changes[0].find("MESSAGE"), std::string::npos);
    EXPECT_NE(changes[0].find("m1"), std::string::npos);
    EXPECT_NE(changes[1].find("m2"), std::string::npos);

    // confirmed_flush_lsn should have advanced past the decoded records.
    const ReplicationSlot* s_after = ReplicationSlotLookup("log_slot");
    ASSERT_NE(s_after, nullptr);
    EXPECT_GT(s_after->confirmed_flush_lsn, slot_lsn_before);
}

TEST_F(ReplicationTest, PgLogicalSlotGetChangesWithMaxMessages) {
    ASSERT_TRUE(ReplicationSlotCreate("log_max", SlotType::kLogical, SlotPersistence::kPersistent,
                                      "pgoutput", "postgres"));
    PgLogicalEmitMessage(false, "p", "a");
    PgLogicalEmitMessage(false, "p", "b");
    PgLogicalEmitMessage(false, "p", "c");

    std::vector<std::string> changes =
        PgLogicalSlotGetChanges("log_max", /*upto_lsn=*/0, /*max=*/2);
    EXPECT_EQ(changes.size(), 2u);
}

TEST_F(ReplicationTest, PgLogicalSlotGetChangesOnEmptyWalReturnsNothing) {
    ASSERT_TRUE(ReplicationSlotCreate("log_empty", SlotType::kLogical, SlotPersistence::kPersistent,
                                      "pgoutput", "postgres"));
    std::vector<std::string> changes =
        PgLogicalSlotGetChanges("log_empty", /*upto_lsn=*/0, /*max=*/0);
    EXPECT_TRUE(changes.empty());
}

TEST_F(ReplicationTest, PgLogicalSlotPeekChangesDoesNotAdvance) {
    ASSERT_TRUE(ReplicationSlotCreate("log_peek", SlotType::kLogical, SlotPersistence::kPersistent,
                                      "pgoutput", "postgres"));
    const ReplicationSlot* s_before = ReplicationSlotLookup("log_peek");
    ASSERT_NE(s_before, nullptr);
    XLogRecPtr lsn_before = s_before->confirmed_flush_lsn;

    PgLogicalEmitMessage(false, "p", "peek_msg");

    // Peek — should return the message but NOT advance confirmed_flush_lsn.
    std::vector<std::string> peeked =
        PgLogicalSlotPeekChanges("log_peek", /*upto_lsn=*/0, /*max=*/0);
    ASSERT_EQ(peeked.size(), 1u);
    EXPECT_NE(peeked[0].find("peek_msg"), std::string::npos);

    const ReplicationSlot* s_after = ReplicationSlotLookup("log_peek");
    ASSERT_NE(s_after, nullptr);
    EXPECT_EQ(s_after->confirmed_flush_lsn, lsn_before);  // unchanged

    // Now GetChanges — should still see the message (slot didn't advance).
    std::vector<std::string> got = PgLogicalSlotGetChanges("log_peek", /*upto_lsn=*/0, /*max=*/0);
    ASSERT_EQ(got.size(), 1u);
}

TEST_F(ReplicationTest, PgLogicalSlotPeekChangesRequiresLogicalSlot) {
    ASSERT_TRUE(
        ReplicationSlotCreate("phys2", SlotType::kPhysical, SlotPersistence::kPersistent, "", ""));
    EXPECT_FALSE(
        NoError([&] { (void)PgLogicalSlotPeekChanges("phys2", /*upto_lsn=*/0, /*max=*/0); }));
}

TEST_F(ReplicationTest, PgLogicalSlotPeekChangesRequiresExistingSlot) {
    EXPECT_FALSE(
        NoError([&] { (void)PgLogicalSlotPeekChanges("nonexistent", /*upto_lsn=*/0, /*max=*/0); }));
}

TEST_F(ReplicationTest, PgLogicalSlotGetChangesSubsequentCallIsEmpty) {
    ASSERT_TRUE(ReplicationSlotCreate("log_inc", SlotType::kLogical, SlotPersistence::kPersistent,
                                      "pgoutput", "postgres"));
    PgLogicalEmitMessage(false, "p", "only");

    // First call decodes the message and advances confirmed_flush_lsn.
    std::vector<std::string> first = PgLogicalSlotGetChanges("log_inc", /*upto_lsn=*/0, /*max=*/0);
    ASSERT_EQ(first.size(), 1u);

    // Second call should see no new messages (slot advanced past them).
    std::vector<std::string> second = PgLogicalSlotGetChanges("log_inc", /*upto_lsn=*/0, /*max=*/0);
    EXPECT_TRUE(second.empty());
}

// ===========================================================================
// Part 8: LogicalRepWorker — pool add / remove / find / ApplyWorkerMain
// ===========================================================================

TEST_F(ReplicationTest, WorkerInitClearsPool) {
    EXPECT_EQ(LogicalRepWorkerCount(), 0);
    EXPECT_EQ(GetLogicalRepWorkerPool()->workers.size(), 0u);
}

TEST_F(ReplicationTest, WorkerAddReturnsIndex) {
    int idx =
        LogicalRepWorkerAdd(/*subid=*/100, /*relid=*/0, LogicalRepWorkerType::kApply, "sub_a");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(LogicalRepWorkerCount(), 1);
    LogicalRepWorker* w = LogicalRepWorkerGetByIndex(idx);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->subid, 100);
    EXPECT_EQ(w->subscription_name, "sub_a");
    EXPECT_EQ(w->type, LogicalRepWorkerType::kApply);
    EXPECT_TRUE(w->in_use);
    EXPECT_NE(w->pid, 0);
}

TEST_F(ReplicationTest, WorkerAddMultipleWorkers) {
    int i1 = LogicalRepWorkerAdd(1, 0, LogicalRepWorkerType::kApply, "a");
    int i2 = LogicalRepWorkerAdd(2, 0, LogicalRepWorkerType::kApply, "b");
    int i3 = LogicalRepWorkerAdd(3, 0, LogicalRepWorkerType::kParallel, "c");
    ASSERT_GE(i1, 0);
    ASSERT_GE(i2, 0);
    ASSERT_GE(i3, 0);
    EXPECT_EQ(LogicalRepWorkerCount(), 3);
    EXPECT_NE(LogicalRepWorkerGetByIndex(i1), nullptr);
    EXPECT_NE(LogicalRepWorkerGetByIndex(i2), nullptr);
    EXPECT_NE(LogicalRepWorkerGetByIndex(i3), nullptr);
}

TEST_F(ReplicationTest, WorkerAddRejectsInvalidSubid) {
    EXPECT_FALSE(NoError([&] { LogicalRepWorkerAdd(0, 0, LogicalRepWorkerType::kApply, "bad"); }));
}

TEST_F(ReplicationTest, WorkerFindBySub) {
    LogicalRepWorkerAdd(111, 0, LogicalRepWorkerType::kApply, "x");
    LogicalRepWorkerAdd(222, 0, LogicalRepWorkerType::kApply, "y");
    EXPECT_EQ(LogicalRepWorkerFindBySub(111), 0);
    EXPECT_EQ(LogicalRepWorkerFindBySub(222), 1);
    EXPECT_EQ(LogicalRepWorkerFindBySub(999), -1);
}

TEST_F(ReplicationTest, WorkerRemoveFreesSlot) {
    int idx = LogicalRepWorkerAdd(1, 0, LogicalRepWorkerType::kApply, "z");
    ASSERT_GE(idx, 0);
    EXPECT_TRUE(LogicalRepWorkerRemove(idx));
    EXPECT_EQ(LogicalRepWorkerCount(), 0);
    // Removed worker is no longer addressable.
    EXPECT_EQ(LogicalRepWorkerGetByIndex(idx), nullptr);
    EXPECT_FALSE(LogicalRepWorkerRemove(-1));
    EXPECT_FALSE(LogicalRepWorkerRemove(999));
}

TEST_F(ReplicationTest, WorkerPoolReusesFreedSlot) {
    int i1 = LogicalRepWorkerAdd(1, 0, LogicalRepWorkerType::kApply, "first");
    ASSERT_EQ(i1, 0);
    ASSERT_TRUE(LogicalRepWorkerRemove(i1));
    // After freeing slot 0, the next add should reuse it (rather than
    // growing the vector).
    int i2 = LogicalRepWorkerAdd(2, 0, LogicalRepWorkerType::kApply, "second");
    EXPECT_EQ(i2, 0);
    LogicalRepWorker* w = LogicalRepWorkerGetByIndex(i2);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->subscription_name, "second");
    EXPECT_EQ(w->subid, 2);
    EXPECT_EQ(GetLogicalRepWorkerPool()->workers.size(), 1u);
}

TEST_F(ReplicationTest, WorkerPoolCapsAtMax) {
    int max = GetLogicalRepWorkerPool()->max_workers;
    for (int i = 0; i < max; ++i) {
        ASSERT_GE(
            LogicalRepWorkerAdd(1000 + i, 0, LogicalRepWorkerType::kApply, "s" + std::to_string(i)),
            0);
    }
    EXPECT_FALSE(
        NoError([&] { LogicalRepWorkerAdd(9999, 0, LogicalRepWorkerType::kApply, "overflow"); }));
}

TEST_F(ReplicationTest, ApplyWorkerMainRunsAndAdvancesCommitLsn) {
    int idx = LogicalRepWorkerAdd(1, 0, LogicalRepWorkerType::kApply, "run");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(ApplyWorkerMain(idx), 0);
    LogicalRepWorker* w = LogicalRepWorkerGetByIndex(idx);
    ASSERT_NE(w, nullptr);
    EXPECT_GT(w->commit_lsn, 0u);
    EXPECT_FALSE(w->running);
}

TEST_F(ReplicationTest, ApplyWorkerMainInvalidIndexErrors) {
    EXPECT_FALSE(NoError([&] { ApplyWorkerMain(-1); }));
}

TEST_F(ReplicationTest, ApplyWorkerWakeupSetsRunningFlag) {
    int idx = LogicalRepWorkerAdd(1, 0, LogicalRepWorkerType::kApply, "wake");
    ASSERT_GE(idx, 0);
    ApplyWorkerWakeup(idx);
    LogicalRepWorker* w = LogicalRepWorkerGetByIndex(idx);
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(w->running);
    ApplyWorkerWakeup(-1);  // no-op, no error
}

// ===========================================================================
// Part 9: ReplOrigin — create / drop / advance / session set/get
// ===========================================================================

TEST_F(ReplicationTest, CreateReturnsUniqueId) {
    RepOriginId id1 = ReploriginCreate("node_a");
    RepOriginId id2 = ReploriginCreate("node_b");
    EXPECT_GE(id1, kFirstUserRepOriginId);
    EXPECT_GT(id2, id1);
    EXPECT_EQ(ReploriginCount(), 2);
}

TEST_F(ReplicationTest, CreateReturnsSameIdForSameName) {
    RepOriginId id1 = ReploriginCreate("dup");
    RepOriginId id2 = ReploriginCreate("dup");
    EXPECT_EQ(id1, id2);
    EXPECT_EQ(ReploriginCount(), 1);
}

TEST_F(ReplicationTest, CreateRejectsEmptyName) {
    EXPECT_FALSE(NoError([&] { ReploriginCreate(""); }));
    EXPECT_EQ(ReploriginCount(), 0);
}

TEST_F(ReplicationTest, LookupByIdAndByName) {
    RepOriginId id = ReploriginCreate("lookup_test");
    const CommitOrigin* o = ReploriginGet(id);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->name, "lookup_test");
    EXPECT_EQ(ReploriginGetByName("lookup_test"), id);
    EXPECT_EQ(ReploriginGetByName("missing"), kInvalidRepOriginId);
    EXPECT_EQ(ReploriginGet(9999), nullptr);
}

TEST_F(ReplicationTest, DropById) {
    RepOriginId id = ReploriginCreate("dropme");
    EXPECT_TRUE(ReploriginDrop(id));
    EXPECT_EQ(ReploriginGet(id), nullptr);
    EXPECT_FALSE(ReploriginDrop(id));
    EXPECT_EQ(ReploriginCount(), 0);
}

TEST_F(ReplicationTest, DropByName) {
    RepOriginId id = ReploriginCreate("by_name");
    EXPECT_TRUE(ReploriginDropByName("by_name"));
    EXPECT_FALSE(ReploriginDropByName("by_name"));
    EXPECT_EQ(ReploriginGet(id), nullptr);
}

TEST_F(ReplicationTest, AdvanceUpdatesLsns) {
    RepOriginId id = ReploriginCreate("adv");
    EXPECT_TRUE(ReploriginAdvance(id, 100, 200));
    const CommitOrigin* o = ReploriginGet(id);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->remote_lsn, 100u);
    EXPECT_EQ(o->local_lsn, 200u);
    // Backward advance is a no-op (max semantics).
    EXPECT_TRUE(ReploriginAdvance(id, 50, 100));
    EXPECT_EQ(o->remote_lsn, 100u);
    EXPECT_EQ(o->local_lsn, 200u);
}

TEST_F(ReplicationTest, AdvanceMissingReturnsFalse) {
    EXPECT_FALSE(ReploriginAdvance(9999, 1, 2));
}

TEST_F(ReplicationTest, SessionSetGetReset) {
    EXPECT_EQ(ReploriginSessionGet(), kInvalidRepOriginId);
    RepOriginId id = ReploriginCreate("sess");
    EXPECT_TRUE(ReploriginSessionSet(id));
    EXPECT_EQ(ReploriginSessionGet(), id);
    ReploriginSessionReset();
    EXPECT_EQ(ReploriginSessionGet(), kInvalidRepOriginId);
}

TEST_F(ReplicationTest, SessionSetRejectsMissingOrigin) {
    EXPECT_FALSE(NoError([&] { ReploriginSessionSet(9999); }));
}

TEST_F(ReplicationTest, SessionSetClearsOnDropOfSessionOrigin) {
    RepOriginId id = ReploriginCreate("s_then_drop");
    ASSERT_TRUE(ReploriginSessionSet(id));
    ASSERT_TRUE(ReploriginDrop(id));
    EXPECT_EQ(ReploriginSessionGet(), kInvalidRepOriginId);
}

TEST_F(ReplicationTest, SessionLsnDefaultsToZero) {
    EXPECT_EQ(ReploriginSessionLsn(), 0u);
}

// ===========================================================================
// Part 10: Launcher — init / main / wakeup / shutdown / id allocation
// ===========================================================================

TEST_F(ReplicationTest, LauncherInitClearsState) {
    EXPECT_FALSE(ApplyLauncherIsRunning());
    auto* s = GetLogicalRepLauncherState();
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->workers_started, 0);
    EXPECT_EQ(s->workers_stopped, 0);
    EXPECT_FALSE(s->shutdown_requested);
}

TEST_F(ReplicationTest, LauncherMainRunsAndStops) {
    int started = ApplyLauncherMain(/*max_iterations=*/5);
    EXPECT_EQ(started, 0);  // stubbed: no workers started
    EXPECT_FALSE(ApplyLauncherIsRunning());
    auto* s = GetLogicalRepLauncherState();
    ASSERT_NE(s, nullptr);
    EXPECT_GT(s->last_run_time_ms, 0);
}

TEST_F(ReplicationTest, LauncherWakeupUpdatesTimestamp) {
    auto* s = GetLogicalRepLauncherState();
    ASSERT_NE(s, nullptr);
    int64_t before = s->last_run_time_ms;
    ApplyLauncherWakeup();
    EXPECT_GE(s->last_run_time_ms, before);
}

TEST_F(ReplicationTest, LauncherShutdownSetsFlag) {
    ApplyLauncherShutdown();
    EXPECT_TRUE(GetLogicalRepLauncherState()->shutdown_requested);
}

TEST_F(ReplicationTest, LauncherMainExitsImmediatelyWhenShutdownRequested) {
    ApplyLauncherShutdown();
    int started = ApplyLauncherMain(/*max_iterations=*/10);
    EXPECT_EQ(started, 0);
}

TEST_F(ReplicationTest, AllocateNextSubscriptionIdIsMonotonic) {
    int64_t id1 = AllocateNextSubscriptionId();
    int64_t id2 = AllocateNextSubscriptionId();
    int64_t id3 = AllocateNextSubscriptionId();
    EXPECT_GT(id2, id1);
    EXPECT_GT(id3, id2);
}

// ===========================================================================
// Part 11: SyncRep — config update / parse / lookup / wait-for-LSN
// ===========================================================================

TEST_F(ReplicationTest, SyncRepInitClearsConfig) {
    const SyncRepConfig* c = SyncRepConfigGet();
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->standby_names.empty());
    EXPECT_EQ(c->num_sync, 0);
    EXPECT_FALSE(c->initialized == false);  // initialized by SyncRepConfigInit
    EXPECT_EQ(SyncRepGetWaiters(), 0);
}

TEST_F(ReplicationTest, SyncRepConfigUpdate) {
    std::vector<std::string> names = {"s1", "s2", "s3"};
    EXPECT_TRUE(SyncRepConfigUpdate(names, /*num_sync=*/2, SyncRepSyncMethod::kPriority));
    const SyncRepConfig* c = SyncRepConfigGet();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->standby_names.size(), 3u);
    EXPECT_EQ(c->num_sync, 2);
    EXPECT_EQ(c->method, SyncRepSyncMethod::kPriority);
}

TEST_F(ReplicationTest, SyncRepConfigUpdateRejectsBadNumSync) {
    std::vector<std::string> names = {"only_one"};
    EXPECT_FALSE(NoError([&] { SyncRepConfigUpdate(names, 5, SyncRepSyncMethod::kPriority); }));
}

TEST_F(ReplicationTest, SyncRepConfigParseSimpleList) {
    ASSERT_TRUE(SyncRepConfigParse("standby_a, standby_b"));
    const SyncRepConfig* c = SyncRepConfigGet();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->standby_names.size(), 2u);
    EXPECT_EQ(c->standby_names[0], "standby_a");
    EXPECT_EQ(c->standby_names[1], "standby_b");
    EXPECT_EQ(c->num_sync, 1);
    EXPECT_EQ(c->method, SyncRepSyncMethod::kPriority);
}

TEST_F(ReplicationTest, SyncRepConfigParseNListForm) {
    ASSERT_TRUE(SyncRepConfigParse("2 (a, b, c)"));
    const SyncRepConfig* c = SyncRepConfigGet();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->num_sync, 2);
    EXPECT_EQ(c->standby_names.size(), 3u);
    EXPECT_EQ(c->method, SyncRepSyncMethod::kPriority);
}

TEST_F(ReplicationTest, SyncRepConfigParseAnyForm) {
    ASSERT_TRUE(SyncRepConfigParse("ANY 2 (a, b, c)"));
    const SyncRepConfig* c = SyncRepConfigGet();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->num_sync, 2);
    EXPECT_EQ(c->method, SyncRepSyncMethod::kQuorum);
    EXPECT_EQ(c->standby_names.size(), 3u);
}

TEST_F(ReplicationTest, SyncRepConfigParseEmptyMeansNoStandbys) {
    ASSERT_TRUE(SyncRepConfigParse(""));
    const SyncRepConfig* c = SyncRepConfigGet();
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->standby_names.empty());
    EXPECT_EQ(c->num_sync, 0);
}

TEST_F(ReplicationTest, SyncRepConfigParseRejectsMissingCloseParen) {
    EXPECT_FALSE(NoError([&] { SyncRepConfigParse("1 (a, b"); }));
}

TEST_F(ReplicationTest, SyncRepConfigParseRejectsBadCount) {
    EXPECT_FALSE(NoError([&] { SyncRepConfigParse("x (a, b)"); }));
}

TEST_F(ReplicationTest, SyncRepConfigParseRejectsCountExceedingNames) {
    EXPECT_FALSE(NoError([&] { SyncRepConfigParse("3 (a, b)"); }));
}

TEST_F(ReplicationTest, SyncRepWaitForLsnReturnsLsnImmediately) {
    EXPECT_EQ(SyncRepWaitForLSN(12345u), 12345u);
    // No waiters should be observed (the stub returns immediately).
    EXPECT_EQ(SyncRepGetWaiters(), 0);
}

TEST_F(ReplicationTest, SyncRepIsSyncStandbyMatchesByName) {
    ASSERT_TRUE(SyncRepConfigParse("a, b"));
    EXPECT_TRUE(SyncRepIsSyncStandby("a"));
    EXPECT_TRUE(SyncRepIsSyncStandby("b"));
    EXPECT_FALSE(SyncRepIsSyncStandby("c"));
}

TEST_F(ReplicationTest, SyncRepIsSyncStandbyMatchesWildcard) {
    ASSERT_TRUE(SyncRepConfigParse("*"));
    EXPECT_TRUE(SyncRepIsSyncStandby("anything"));
    EXPECT_TRUE(SyncRepIsSyncStandby("also_ok"));
}

// ===========================================================================
// Part 12: Backup — start / do / stop
// ===========================================================================

TEST_F(ReplicationTest, BackupStartStopLifecycle) {
    BackupHandle h = StartBackup("test_backup");
    EXPECT_EQ(h.state, BackupState::kRunning);
    EXPECT_EQ(h.label, "test_backup");
    EXPECT_GT(h.start_lsn, 0u);

    EXPECT_NE(GetCurrentBackup(), nullptr);

    XLogRecPtr end = StopBackup(h);
    EXPECT_GT(end, 0u);
    EXPECT_EQ(h.state, BackupState::kDone);
    EXPECT_EQ(GetCurrentBackup(), nullptr);
}

TEST_F(ReplicationTest, BackupStartRejectsDoubleStart) {
    BackupHandle h1 = StartBackup("first");
    ASSERT_EQ(h1.state, BackupState::kRunning);
    EXPECT_FALSE(NoError([&] { StartBackup("second"); }));
    StopBackup(h1);
}

TEST_F(ReplicationTest, DoBackupRecordsFiles) {
    BackupHandle h = StartBackup("do_test");
    int written = DoBackup(h, {{"file1", 100}, {"file2", 200}});
    EXPECT_EQ(written, 2);
    EXPECT_EQ(h.files.size(), 2u);
    EXPECT_EQ(h.total_bytes, 300u);
    StopBackup(h);
}

TEST_F(ReplicationTest, DoBackupRejectsWhenNotRunning) {
    BackupHandle h;  // not running
    EXPECT_FALSE(NoError([&] { DoBackup(h, {{"f", 1}}); }));
}

TEST_F(ReplicationTest, StopBackupRejectsWhenNotRunning) {
    BackupHandle h;
    EXPECT_FALSE(NoError([&] { StopBackup(h); }));
}

TEST_F(ReplicationTest, BackupStateName) {
    EXPECT_STREQ(BackupStateName(BackupState::kNotStarted), "not_started");
    EXPECT_STREQ(BackupStateName(BackupState::kRunning), "running");
    EXPECT_STREQ(BackupStateName(BackupState::kDone), "done");
    EXPECT_STREQ(BackupStateName(BackupState::kCancelled), "cancelled");
}
