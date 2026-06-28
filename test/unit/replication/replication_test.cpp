// replication_test.cpp — Unit tests for the MyToyDB replication subsystem
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

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/replication/backup.hpp"
#include "pgcpp/replication/launcher.hpp"
#include "pgcpp/replication/logical.hpp"
#include "pgcpp/replication/origin.hpp"
#include "pgcpp/replication/replutil.hpp"
#include "pgcpp/replication/slot.hpp"
#include "pgcpp/replication/slotfuncs.hpp"
#include "pgcpp/replication/syncrep.hpp"
#include "pgcpp/replication/walreceiver.hpp"
#include "pgcpp/replication/walsender.hpp"
#include "pgcpp/replication/walsenderfuncs.hpp"
#include "pgcpp/replication/worker.hpp"
#include "pgcpp/transaction/xlog.hpp"

using mytoydb::replication::AllocateNextSubscriptionId;
using mytoydb::replication::ApplyLauncherInit;
using mytoydb::replication::ApplyLauncherIsRunning;
using mytoydb::replication::ApplyLauncherMain;
using mytoydb::replication::ApplyLauncherReset;
using mytoydb::replication::ApplyLauncherShutdown;
using mytoydb::replication::ApplyLauncherWakeup;
using mytoydb::replication::ApplyStandbyReply;
using mytoydb::replication::ApplyWorkerMain;
using mytoydb::replication::ApplyWorkerWakeup;
using mytoydb::replication::BackupHandle;
using mytoydb::replication::BackupState;
using mytoydb::replication::BackupStateName;
using mytoydb::replication::CommitOrigin;
using mytoydb::replication::CreateDecodingContext;
using mytoydb::replication::CreateInitDecodingContext;
using mytoydb::replication::DecodingEmitMessage;
using mytoydb::replication::DefaultOutputPluginName;
using mytoydb::replication::DoBackup;
using mytoydb::replication::GetCurrentBackup;
using mytoydb::replication::GetLogicalDecodingContext;
using mytoydb::replication::GetLogicalRepLauncherState;
using mytoydb::replication::GetLogicalRepWorkerPool;
using mytoydb::replication::GetReplicationSlotCtl;
using mytoydb::replication::GetWalLevel;
using mytoydb::replication::GetWalRcvData;
using mytoydb::replication::GetWalSndCtl;
using mytoydb::replication::GetWalSndStats;
using mytoydb::replication::InitializeBackup;
using mytoydb::replication::kFirstUserRepOriginId;
using mytoydb::replication::kInvalidRepOriginId;
using mytoydb::replication::kMaxWalSenders;
using mytoydb::replication::LogicalDecodingContext;
using mytoydb::replication::LogicalDecodingOptions;
using mytoydb::replication::LogicalDecodingReset;
using mytoydb::replication::LogicalRepMsgType;
using mytoydb::replication::LogicalRepMsgTypeName;
using mytoydb::replication::LogicalRepWorker;
using mytoydb::replication::LogicalRepWorkerAdd;
using mytoydb::replication::LogicalRepWorkerCount;
using mytoydb::replication::LogicalRepWorkerFindBySub;
using mytoydb::replication::LogicalRepWorkerGetByIndex;
using mytoydb::replication::LogicalRepWorkerInit;
using mytoydb::replication::LogicalRepWorkerRemove;
using mytoydb::replication::LogicalRepWorkerReset;
using mytoydb::replication::LogicalRepWorkerType;
using mytoydb::replication::LogicalShippingMain;
using mytoydb::replication::PgCreateReplicationSlot;
using mytoydb::replication::PgCreateReplicationSlotResult;
using mytoydb::replication::PgDropReplicationSlot;
using mytoydb::replication::PgReplicationSlotAdvance;
using mytoydb::replication::PgReplicationSlotAdvanceToCurrent;
using mytoydb::replication::ReplicationSlot;
using mytoydb::replication::ReplicationSlotAcquire;
using mytoydb::replication::ReplicationSlotAdvance;
using mytoydb::replication::ReplicationSlotCount;
using mytoydb::replication::ReplicationSlotCreate;
using mytoydb::replication::ReplicationSlotDrop;
using mytoydb::replication::ReplicationSlotInit;
using mytoydb::replication::ReplicationSlotLookup;
using mytoydb::replication::ReplicationSlotPersist;
using mytoydb::replication::ReplicationSlotRelease;
using mytoydb::replication::ReploriginAdvance;
using mytoydb::replication::ReploriginCount;
using mytoydb::replication::ReploriginCreate;
using mytoydb::replication::ReploriginDrop;
using mytoydb::replication::ReploriginDropByName;
using mytoydb::replication::ReploriginGet;
using mytoydb::replication::ReploriginGetByName;
using mytoydb::replication::ReplOriginInit;
using mytoydb::replication::ReplOriginReset;
using mytoydb::replication::ReploriginSessionGet;
using mytoydb::replication::ReploriginSessionLsn;
using mytoydb::replication::ReploriginSessionReset;
using mytoydb::replication::ReploriginSessionSet;
using mytoydb::replication::RepOriginId;
using mytoydb::replication::SlotPersistence;
using mytoydb::replication::SlotPersistenceName;
using mytoydb::replication::SlotType;
using mytoydb::replication::SlotTypeName;
using mytoydb::replication::StartBackup;
using mytoydb::replication::StopBackup;
using mytoydb::replication::SyncRepConfig;
using mytoydb::replication::SyncRepConfigGet;
using mytoydb::replication::SyncRepConfigInit;
using mytoydb::replication::SyncRepConfigParse;
using mytoydb::replication::SyncRepConfigReset;
using mytoydb::replication::SyncRepConfigUpdate;
using mytoydb::replication::SyncRepGetWaiters;
using mytoydb::replication::SyncRepIsSyncStandby;
using mytoydb::replication::SyncRepSyncMethod;
using mytoydb::replication::SyncRepWaitForLSN;
using mytoydb::replication::WalLevel;
using mytoydb::replication::WalRcvData;
using mytoydb::replication::WalRcvGetState;
using mytoydb::replication::WalRcvGetStreamState;
using mytoydb::replication::WalRcvInit;
using mytoydb::replication::WalRcvLsnKind;
using mytoydb::replication::WalRcvReportLsn;
using mytoydb::replication::WalRcvSetPid;
using mytoydb::replication::WalRcvStart;
using mytoydb::replication::WalRcvState;
using mytoydb::replication::WalRcvStateName;
using mytoydb::replication::WalRcvStop;
using mytoydb::replication::WalSnd;
using mytoydb::replication::WalSndAlloc;
using mytoydb::replication::WalSndCount;
using mytoydb::replication::WalSndCtlData;
using mytoydb::replication::WalSndGetByIndex;
using mytoydb::replication::WalSndGetByPid;
using mytoydb::replication::WalSndGetState;
using mytoydb::replication::WalSndInit;
using mytoydb::replication::WalSndKeepalive;
using mytoydb::replication::WalSndLsnKind;
using mytoydb::replication::WalSndMessageResult;
using mytoydb::replication::WalSndRemove;
using mytoydb::replication::WalSndReplyMessage;
using mytoydb::replication::WalSndSetState;
using mytoydb::replication::WalSndState;
using mytoydb::replication::WalSndStats;
using mytoydb::replication::WalSndUpdateLsn;
using mytoydb::replication::WalSndWaitForWal;
using mytoydb::replication::WalSndWakeup;
using mytoydb::replication::WalSndWriteData;
using mytoydb::transaction::GetXLogInsertRecPtr;
using mytoydb::transaction::InitializeWal;
using mytoydb::transaction::ResetWal;
using mytoydb::transaction::XLogRecPtr;

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
        mytoydb::error::InitErrorSubsystem();
        context_ = mytoydb::memory::AllocSetContext::Create("repl_test_ctx");
        mytoydb::memory::SetCurrentMemoryContext(context_);

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
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    mytoydb::memory::MemoryContext* context_ = nullptr;
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
    mytoydb::replication::SetWalLevel(WalLevel::kLogical);
    EXPECT_EQ(GetWalLevel(), WalLevel::kLogical);
    mytoydb::replication::SetWalLevel(WalLevel::kReplica);
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
    EXPECT_EQ(WalRcvGetStreamState(), mytoydb::replication::WalRcvStreamState::kNone);
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

TEST_F(ReplicationTest, LogicalShippingMainEmitsBeginCommitPairs) {
    LogicalDecodingContext ctx =
        CreateInitDecodingContext("pgoutput", "ship", "postgres", LogicalDecodingOptions{});
    int emitted = LogicalShippingMain(ctx, /*max_messages=*/3);
    EXPECT_EQ(emitted, 6);  // 3 BEGIN + 3 COMMIT
    EXPECT_EQ(ctx.messages.size(), 6u);
    EXPECT_GT(ctx.end_lsn, 0u);
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
