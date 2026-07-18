// pg_recvlogical_test.cpp — Unit tests for the pg_recvlogical tool.
//
// Verifies the SQL builders, the logical-message parser (BEGIN/COMMIT/
// INSERT/UPDATE/DELETE/TRUNCATE), and the error paths of RunRecvlogical
// (no server reachable, so most paths hit kConnectFailed).
#include "tools/pg_recvlogical.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace pt = pgcpp::tools;
using std::string;

TEST(PgRecvlogicalTest, BuildCreateSlotSqlShape) {
    string sql = pt::BuildCreateSlotSql("myslot", "pgoutput");
    EXPECT_NE(sql.find("pg_create_logical_replication_slot"), string::npos);
    EXPECT_NE(sql.find("'myslot'"), string::npos);
    EXPECT_NE(sql.find("'pgoutput'"), string::npos);
}

TEST(PgRecvlogicalTest, BuildCreateSlotSqlEscapesQuotes) {
    string sql = pt::BuildCreateSlotSql("my'slot", "test'plugin");
    EXPECT_NE(sql.find("'my''slot'"), string::npos);
    EXPECT_NE(sql.find("'test''plugin'"), string::npos);
}

TEST(PgRecvlogicalTest, BuildDropSlotSqlShape) {
    string sql = pt::BuildDropSlotSql("myslot");
    EXPECT_NE(sql.find("pg_drop_replication_slot"), string::npos);
    EXPECT_NE(sql.find("'myslot'"), string::npos);
}

TEST(PgRecvlogicalTest, BuildSlotInfoSqlShape) {
    string sql = pt::BuildSlotInfoSql("myslot");
    EXPECT_NE(sql.find("pg_replication_slots"), string::npos);
    EXPECT_NE(sql.find("WHERE slot_name = 'myslot'"), string::npos);
    EXPECT_NE(sql.find("slot_name"), string::npos);
    EXPECT_NE(sql.find("plugin"), string::npos);
    EXPECT_NE(sql.find("slot_type"), string::npos);
}

TEST(PgRecvlogicalTest, BuildStartReplicationSqlNullLsn) {
    string sql = pt::BuildStartReplicationSql("myslot", 0, 100);
    EXPECT_NE(sql.find("pg_logical_slot_get_changes"), string::npos);
    EXPECT_NE(sql.find("'myslot'"), string::npos);
    EXPECT_NE(sql.find("NULL"), string::npos);
    EXPECT_NE(sql.find("100"), string::npos);
}

TEST(PgRecvlogicalTest, BuildStartReplicationSqlWithLsn) {
    string sql = pt::BuildStartReplicationSql("myslot", 12345, 50);
    EXPECT_NE(sql.find("'12345'"), string::npos);
    EXPECT_NE(sql.find("50"), string::npos);
}

TEST(PgRecvlogicalTest, BuildSlotPeekSqlShape) {
    string sql = pt::BuildSlotPeekSql("myslot", 0, 100);
    EXPECT_NE(sql.find("pg_logical_slot_peek_changes"), string::npos);
}

TEST(PgRecvlogicalTest, BuildAdvanceSlotSqlShape) {
    string sql = pt::BuildAdvanceSlotSql("myslot", 99999);
    EXPECT_NE(sql.find("pg_replication_slot_advance"), string::npos);
    EXPECT_NE(sql.find("'99999'"), string::npos);
    EXPECT_NE(sql.find("'myslot'"), string::npos);
}

TEST(PgRecvlogicalTest, ParseMsgTypeKnownTypes) {
    std::vector<uint8_t> b = {'B'};
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kBegin);
    b[0] = 'C';
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kCommit);
    b[0] = 'O';
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kOrigin);
    b[0] = 'R';
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kRelation);
    b[0] = 'Y';
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kType);
    b[0] = 'I';
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kInsert);
    b[0] = 'U';
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kUpdate);
    b[0] = 'D';
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kDelete);
    b[0] = 'T';
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kTruncate);
}

TEST(PgRecvlogicalTest, ParseMsgTypeEmptyBuffer) {
    std::vector<uint8_t> b;
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kUnknown);
}

TEST(PgRecvlogicalTest, ParseMsgTypeUnknownType) {
    std::vector<uint8_t> b = {'Z'};
    EXPECT_EQ(pt::ParseMsgType(b), pt::LogicalMsgType::kUnknown);
}

TEST(PgRecvlogicalTest, ParseBeginMessageValid) {
    // Type(1) + LSN(8) + EndLSN(8) + TimeSec(4) + TimeUsec(4) + XID(4)
    std::vector<uint8_t> buf;
    buf.push_back('B');
    // LSN = 0x0163D40000000001
    buf.push_back(0x01);
    buf.push_back(0x63);
    buf.push_back(0xD4);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    // EndLSN
    for (int i = 0; i < 8; ++i)
        buf.push_back(0);
    // TimeSec
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(100);
    // TimeUsec
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    // XID = 0x000004D2 = 1234
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0x04);
    buf.push_back(0xD2);

    std::int64_t lsn = 0;
    std::int64_t xid = 0;
    ASSERT_TRUE(pt::ParseBeginMessage(buf, &lsn, &xid));
    EXPECT_EQ(xid, 1234);
    EXPECT_NE(lsn, 0);
}

TEST(PgRecvlogicalTest, ParseBeginMessageTooShort) {
    std::vector<uint8_t> buf = {'B', 1, 2, 3};
    std::int64_t lsn = 0;
    std::int64_t xid = 0;
    EXPECT_FALSE(pt::ParseBeginMessage(buf, &lsn, &xid));
}

TEST(PgRecvlogicalTest, ParseBeginMessageWrongType) {
    std::vector<uint8_t> buf(30, 0);
    buf[0] = 'C';  // wrong type
    std::int64_t lsn = 0;
    std::int64_t xid = 0;
    EXPECT_FALSE(pt::ParseBeginMessage(buf, &lsn, &xid));
}

TEST(PgRecvlogicalTest, ParseCommitMessageValid) {
    // Type(1) + Flags(1) + LSN(8) + EndLSN(8) + Time(8).
    std::vector<uint8_t> buf;
    buf.push_back('C');
    buf.push_back(0);  // flags
    // LSN = 0x0102030405060708
    for (int b : {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08})
        buf.push_back(static_cast<uint8_t>(b));
    // EndLSN
    for (int i = 0; i < 8; ++i)
        buf.push_back(0);
    // Time
    for (int i = 0; i < 8; ++i)
        buf.push_back(0);

    std::int64_t lsn = 0;
    ASSERT_TRUE(pt::ParseCommitMessage(buf, &lsn));
    EXPECT_EQ(lsn, 0x0102030405060708LL);
}

TEST(PgRecvlogicalTest, ParseCommitMessageWrongType) {
    std::vector<uint8_t> buf(30, 0);
    buf[0] = 'B';  // wrong type
    std::int64_t lsn = 0;
    EXPECT_FALSE(pt::ParseCommitMessage(buf, &lsn));
}

TEST(PgRecvlogicalTest, ParseInsertMessageValid) {
    // Type(1) + Flags(1) + RelOID(4).
    std::vector<uint8_t> buf;
    buf.push_back('I');
    buf.push_back(0);  // flags
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0x16);
    buf.push_back(0x3E);  // 0x163E = 5694

    uint32_t oid = 0;
    ASSERT_TRUE(pt::ParseInsertMessage(buf, &oid));
    EXPECT_EQ(oid, 0x163EU);
}

TEST(PgRecvlogicalTest, ParseInsertMessageTooShort) {
    std::vector<uint8_t> buf = {'I', 0};
    uint32_t oid = 0;
    EXPECT_FALSE(pt::ParseInsertMessage(buf, &oid));
}

TEST(PgRecvlogicalTest, ParseUpdateMessageValid) {
    std::vector<uint8_t> buf;
    buf.push_back('U');
    buf.push_back(0);
    buf.push_back(0x12);
    buf.push_back(0x34);
    buf.push_back(0x56);
    buf.push_back(0x78);
    uint32_t oid = 0;
    ASSERT_TRUE(pt::ParseUpdateMessage(buf, &oid));
    EXPECT_EQ(oid, 0x12345678U);
}

TEST(PgRecvlogicalTest, ParseDeleteMessageValid) {
    std::vector<uint8_t> buf;
    buf.push_back('D');
    buf.push_back(0);
    buf.push_back(0xFF);
    buf.push_back(0xEE);
    buf.push_back(0xDD);
    buf.push_back(0xCC);
    uint32_t oid = 0;
    ASSERT_TRUE(pt::ParseDeleteMessage(buf, &oid));
    EXPECT_EQ(oid, 0xFFEEDDCCU);
}

TEST(PgRecvlogicalTest, ParseTruncateMessageValid) {
    // Type(1) + Flags(1) + Count(4) + RelOID[count]
    std::vector<uint8_t> buf;
    buf.push_back('T');
    buf.push_back(0);  // flags
    // count = 2
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(2);
    // OID 1 = 0x10000001
    buf.push_back(0x10);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    // OID 2 = 0x20000002
    buf.push_back(0x20);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x02);
    std::vector<uint32_t> oids;
    ASSERT_TRUE(pt::ParseTruncateMessage(buf, &oids));
    ASSERT_EQ(oids.size(), 2U);
    EXPECT_EQ(oids[0], 0x10000001U);
    EXPECT_EQ(oids[1], 0x20000002U);
}

TEST(PgRecvlogicalTest, ParseTruncateMessageTooFewOids) {
    // Says count=2 but only provides 1 OID.
    std::vector<uint8_t> buf;
    buf.push_back('T');
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(2);  // count
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(1);  // 1 OID
    std::vector<uint32_t> oids;
    EXPECT_FALSE(pt::ParseTruncateMessage(buf, &oids));
}

TEST(PgRecvlogicalTest, MsgTypeNameKnown) {
    EXPECT_STREQ(pt::MsgTypeName(pt::LogicalMsgType::kBegin), "BEGIN");
    EXPECT_STREQ(pt::MsgTypeName(pt::LogicalMsgType::kCommit), "COMMIT");
    EXPECT_STREQ(pt::MsgTypeName(pt::LogicalMsgType::kInsert), "INSERT");
    EXPECT_STREQ(pt::MsgTypeName(pt::LogicalMsgType::kUpdate), "UPDATE");
    EXPECT_STREQ(pt::MsgTypeName(pt::LogicalMsgType::kDelete), "DELETE");
    EXPECT_STREQ(pt::MsgTypeName(pt::LogicalMsgType::kTruncate), "TRUNCATE");
    EXPECT_STREQ(pt::MsgTypeName(pt::LogicalMsgType::kUnknown), "UNKNOWN");
}

TEST(PgRecvlogicalTest, RunRecvlogicalEmptySlotReturnsSlotMissing) {
    pt::RecvlogicalOptions opts;
    opts.slot = "";
    opts.host = "127.0.0.1";
    opts.port = 1;  // unusable
    pt::RecvlogicalStats stats;
    pt::RecvlogicalResult r = pt::RunRecvlogical(opts, stats, nullptr);
    EXPECT_EQ(r, pt::RecvlogicalResult::kSlotMissing);
}

TEST(PgRecvlogicalTest, RunRecvlogicalConnectFailed) {
    pt::RecvlogicalOptions opts;
    opts.slot = "test_slot";
    opts.host = "127.0.0.1";
    opts.port = 1;  // unusable port
    opts.dbname = "postgres";
    pt::RecvlogicalStats stats;
    pt::RecvlogicalResult r = pt::RunRecvlogical(opts, stats, nullptr);
    EXPECT_EQ(r, pt::RecvlogicalResult::kConnectFailed);
}

TEST(PgRecvlogicalTest, RunRecvlogicalCreateConnectFailed) {
    pt::RecvlogicalOptions opts;
    opts.action = pt::RecvlogicalAction::kCreate;
    opts.slot = "test_slot";
    opts.host = "127.0.0.1";
    opts.port = 1;
    opts.dbname = "postgres";
    pt::RecvlogicalStats stats;
    pt::RecvlogicalResult r = pt::RunRecvlogical(opts, stats, nullptr);
    EXPECT_EQ(r, pt::RecvlogicalResult::kConnectFailed);
}

TEST(PgRecvlogicalTest, RunRecvlogicalStopIsNoOp) {
    // Stop is a no-op even without a connection.
    pt::RecvlogicalOptions opts;
    opts.action = pt::RecvlogicalAction::kStop;
    opts.slot = "test_slot";
    opts.host = "127.0.0.1";
    opts.port = 1;
    pt::RecvlogicalStats stats;
    pt::RecvlogicalResult r = pt::RunRecvlogical(opts, stats, nullptr);
    EXPECT_EQ(r, pt::RecvlogicalResult::kConnectFailed);  // still needs to connect
}
