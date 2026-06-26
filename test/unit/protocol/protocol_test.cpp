// protocol_test.cpp — Unit tests for the frontend protocol module (M11).
//
// Tests two areas:
//   1. pqformat: message encoding/decoding (MessageWriter, MessageReader,
//      message builders, wire format).
//   2. postgres: simple and extended query protocol, integrating the full
//      pipeline (parser -> analyzer -> planner -> executor).
//
// The protocol fixture sets up the full stack: error subsystem, memory context,
// catalog + syscache, transaction system, buffer pool, storage directory,
// and relcache. Each test creates fresh relations with known schemas.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "mytoydb/access/heapam.h"
#include "mytoydb/access/rel.h"
#include "mytoydb/catalog/bootstrap_catalog.h"
#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/pg_attribute.h"
#include "mytoydb/catalog/pg_class.h"
#include "mytoydb/catalog/syscache.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/protocol/postgres.h"
#include "mytoydb/protocol/pqformat.h"
#include "mytoydb/storage/bufmgr.h"
#include "mytoydb/storage/smgr.h"
#include "mytoydb/transaction/heap_tuple.h"
#include "mytoydb/transaction/snapshot.h"
#include "mytoydb/transaction/transam.h"
#include "mytoydb/transaction/xact.h"
#include "mytoydb/types/datum.h"

using mytoydb::access::CreateTupleDesc;
using mytoydb::access::heap_form_tuple;
using mytoydb::access::heap_freetuple;
using mytoydb::access::heap_insert;
using mytoydb::access::InitializeRelcache;
using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationCreateStorage;
using mytoydb::access::RelationOpen;
using mytoydb::access::ResetRelcache;
using mytoydb::access::TupleDesc;
using mytoydb::catalog::AttAlign;
using mytoydb::catalog::AttStorage;
using mytoydb::catalog::BootstrapCatalog;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::kFirstNormalObjectId;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::RelKind;
using mytoydb::catalog::RelPersistence;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::memory::AllocSetContext;
using mytoydb::memory::palloc;
using mytoydb::protocol::Backend;
using mytoydb::protocol::BuildBindComplete;
using mytoydb::protocol::BuildCloseComplete;
using mytoydb::protocol::BuildCommandComplete;
using mytoydb::protocol::BuildDataRow;
using mytoydb::protocol::BuildEmptyQueryResponse;
using mytoydb::protocol::BuildErrorResponse;
using mytoydb::protocol::BuildNoData;
using mytoydb::protocol::BuildParseComplete;
using mytoydb::protocol::BuildReadyForQuery;
using mytoydb::protocol::BuildRowDescription;
using mytoydb::protocol::DescribeKind;
using mytoydb::protocol::Message;
using mytoydb::protocol::MessageReader;
using mytoydb::protocol::MessageType;
using mytoydb::protocol::MessageWriter;
using mytoydb::protocol::RowDescriptionField;
using mytoydb::protocol::StringSink;
using mytoydb::protocol::TransactionStatus;
using mytoydb::storage::InitBufferPool;
using mytoydb::storage::SetStorageBaseDir;
using mytoydb::storage::ShutdownBufferPool;
using mytoydb::storage::smgrcloseall;
using mytoydb::transaction::BeginTransactionBlock;
using mytoydb::transaction::EndTransactionBlock;
using mytoydb::transaction::HeapTuple;
using mytoydb::transaction::InitializeSnapshotManager;
using mytoydb::transaction::InitializeTransactionSystem;
using mytoydb::transaction::ResetTransactionState;
using mytoydb::types::Datum;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::kInt4Oid;

namespace {

using mytoydb::nodes::makePallocNode;

// ===========================================================================
// Part 1: pqformat tests — message encoding/decoding (no full stack needed)
// ===========================================================================

TEST(PqFormatTest, MessageWriter_WriteByte) {
    MessageWriter w;
    w.WriteByte('A');
    w.WriteByte('B');
    EXPECT_EQ(w.data(), "AB");
}

TEST(PqFormatTest, MessageWriter_WriteInt16) {
    MessageWriter w;
    w.WriteInt16(0x0102);
    // Network byte order: 0x01, 0x02
    EXPECT_EQ(w.data(), std::string("\x01\x02", 2));
}

TEST(PqFormatTest, MessageWriter_WriteInt32) {
    MessageWriter w;
    w.WriteInt32(0x01020304);
    // Network byte order: 0x01, 0x02, 0x03, 0x04
    EXPECT_EQ(w.data(), std::string("\x01\x02\x03\x04", 4));
}

TEST(PqFormatTest, MessageWriter_WriteString) {
    MessageWriter w;
    w.WriteString("hello");
    // String + NUL terminator
    EXPECT_EQ(w.data(), std::string("hello\0", 6));
}

TEST(PqFormatTest, MessageWriter_WriteBytes) {
    MessageWriter w;
    w.WriteBytes("abc", 3);
    EXPECT_EQ(w.data(), "abc");
}

TEST(PqFormatTest, MessageReader_ReadByte) {
    MessageReader r("AB");
    EXPECT_EQ(r.ReadByte(), 'A');
    EXPECT_EQ(r.ReadByte(), 'B');
    EXPECT_TRUE(r.eof());
}

TEST(PqFormatTest, MessageReader_ReadInt16) {
    MessageReader r(std::string("\x01\x02", 2));
    EXPECT_EQ(r.ReadInt16(), 0x0102);
    EXPECT_TRUE(r.eof());
}

TEST(PqFormatTest, MessageReader_ReadInt32) {
    MessageReader r(std::string("\x01\x02\x03\x04", 4));
    EXPECT_EQ(r.ReadInt32(), 0x01020304);
    EXPECT_TRUE(r.eof());
}

TEST(PqFormatTest, MessageReader_ReadString) {
    MessageReader r(std::string("hello\0world\0", 12));
    EXPECT_EQ(r.ReadString(), "hello");
    EXPECT_EQ(r.ReadString(), "world");
}

TEST(PqFormatTest, MessageReader_ReadBytes) {
    MessageReader r("abcdef");
    EXPECT_EQ(r.ReadBytes(3), "abc");
    EXPECT_EQ(r.remaining(), 3u);
    EXPECT_EQ(r.ReadBytes(3), "def");
    EXPECT_TRUE(r.eof());
}

TEST(PqFormatTest, Message_BuildWireFormat) {
    Message msg(MessageType::kQuery, std::string("SELECT 1\0", 9));
    std::string wire = msg.BuildWireFormat();
    // Type byte 'Q' + length (4+9=13) + payload
    ASSERT_EQ(wire.size(), 1u + 4u + 9u);
    EXPECT_EQ(wire[0], 'Q');
    // Length in network byte order: 13 = 0x0000000D
    EXPECT_EQ(static_cast<uint8_t>(wire[1]), 0x00);
    EXPECT_EQ(static_cast<uint8_t>(wire[2]), 0x00);
    EXPECT_EQ(static_cast<uint8_t>(wire[3]), 0x00);
    EXPECT_EQ(static_cast<uint8_t>(wire[4]), 0x0D);
    EXPECT_EQ(wire.substr(5), std::string("SELECT 1\0", 9));
}

TEST(PqFormatTest, BuildRowDescription) {
    std::vector<RowDescriptionField> fields = {
        {"a", 16384, 1, kInt4Oid, 4, -1, 0},
        {"b", 16384, 2, kInt4Oid, 4, -1, 0},
    };
    Message msg = BuildRowDescription(fields);
    EXPECT_EQ(msg.type, MessageType::kRowDescription);

    // Parse the payload.
    MessageReader r(msg.payload);
    EXPECT_EQ(r.ReadInt16(), 2);  // 2 fields
    // Field 1
    EXPECT_EQ(r.ReadString(), "a");
    EXPECT_EQ(r.ReadInt32(), 16384);                           // table OID
    EXPECT_EQ(r.ReadInt16(), 1);                               // column attnum
    EXPECT_EQ(r.ReadInt32(), static_cast<int32_t>(kInt4Oid));  // type OID
    EXPECT_EQ(r.ReadInt16(), 4);                               // type size
    EXPECT_EQ(r.ReadInt32(), -1);                              // type mod
    EXPECT_EQ(r.ReadInt16(), 0);                               // format (text)
    // Field 2
    EXPECT_EQ(r.ReadString(), "b");
    EXPECT_EQ(r.ReadInt32(), 16384);
    EXPECT_EQ(r.ReadInt16(), 2);
    EXPECT_EQ(r.ReadInt32(), static_cast<int32_t>(kInt4Oid));
    EXPECT_EQ(r.ReadInt16(), 4);
    EXPECT_EQ(r.ReadInt32(), -1);
    EXPECT_EQ(r.ReadInt16(), 0);
}

TEST(PqFormatTest, BuildDataRow) {
    std::vector<std::string> values = {"42", "hello"};
    std::vector<bool> isnull = {false, false};
    Message msg = BuildDataRow(values, isnull);
    EXPECT_EQ(msg.type, MessageType::kDataRow);

    MessageReader r(msg.payload);
    EXPECT_EQ(r.ReadInt16(), 2);  // 2 columns
    // Column 1: "42" (length 2)
    EXPECT_EQ(r.ReadInt32(), 2);
    EXPECT_EQ(r.ReadBytes(2), "42");
    // Column 2: "hello" (length 5)
    EXPECT_EQ(r.ReadInt32(), 5);
    EXPECT_EQ(r.ReadBytes(5), "hello");
}

TEST(PqFormatTest, BuildDataRow_WithNull) {
    std::vector<std::string> values = {"42", ""};
    std::vector<bool> isnull = {false, true};
    Message msg = BuildDataRow(values, isnull);

    MessageReader r(msg.payload);
    EXPECT_EQ(r.ReadInt16(), 2);
    // Column 1: "42"
    EXPECT_EQ(r.ReadInt32(), 2);
    EXPECT_EQ(r.ReadBytes(2), "42");
    // Column 2: NULL (length -1)
    EXPECT_EQ(r.ReadInt32(), -1);
}

TEST(PqFormatTest, BuildCommandComplete) {
    Message msg = BuildCommandComplete("SELECT 3");
    EXPECT_EQ(msg.type, MessageType::kCommandComplete);
    MessageReader r(msg.payload);
    EXPECT_EQ(r.ReadString(), "SELECT 3");
}

TEST(PqFormatTest, BuildEmptyQueryResponse) {
    Message msg = BuildEmptyQueryResponse();
    EXPECT_EQ(msg.type, MessageType::kEmptyQueryResponse);
    EXPECT_TRUE(msg.payload.empty());
}

TEST(PqFormatTest, BuildReadyForQuery) {
    Message msg = BuildReadyForQuery(TransactionStatus::kIdle);
    EXPECT_EQ(msg.type, MessageType::kReadyForQuery);
    MessageReader r(msg.payload);
    EXPECT_EQ(r.ReadByte(), 'I');  // Idle
}

TEST(PqFormatTest, BuildReadyForQuery_InTransaction) {
    Message msg = BuildReadyForQuery(TransactionStatus::kInTransaction);
    MessageReader r(msg.payload);
    EXPECT_EQ(r.ReadByte(), 'T');  // In transaction
}

TEST(PqFormatTest, BuildErrorResponse) {
    Message msg = BuildErrorResponse("relation does not exist");
    EXPECT_EQ(msg.type, MessageType::kErrorResponse);
    // The payload should contain the error message.
    EXPECT_NE(msg.payload.find("relation does not exist"), std::string::npos);
}

TEST(PqFormatTest, BuildParseComplete) {
    Message msg = BuildParseComplete();
    EXPECT_EQ(msg.type, MessageType::kParseComplete);
    EXPECT_TRUE(msg.payload.empty());
}

TEST(PqFormatTest, BuildBindComplete) {
    Message msg = BuildBindComplete();
    EXPECT_EQ(msg.type, MessageType::kBindComplete);
    EXPECT_TRUE(msg.payload.empty());
}

TEST(PqFormatTest, BuildCloseComplete) {
    Message msg = BuildCloseComplete();
    EXPECT_EQ(msg.type, MessageType::kCloseComplete);
    EXPECT_TRUE(msg.payload.empty());
}

TEST(PqFormatTest, BuildNoData) {
    Message msg = BuildNoData();
    EXPECT_EQ(msg.type, MessageType::kNoData);
    EXPECT_TRUE(msg.payload.empty());
}

TEST(PqFormatTest, StringSink_CollectsMessages) {
    StringSink sink;
    sink.SendMessage(BuildCommandComplete("SELECT 1"));
    sink.SendMessage(BuildCommandComplete("SELECT 2"));
    EXPECT_EQ(sink.size(), 2u);
    EXPECT_EQ(sink.at(0).type, MessageType::kCommandComplete);
    EXPECT_EQ(sink.at(1).type, MessageType::kCommandComplete);
}

TEST(PqFormatTest, MessageWriter_RoundTrip) {
    // Write a complex message and read it back.
    MessageWriter w;
    w.WriteInt32(42);
    w.WriteString("test");
    w.WriteInt16(7);
    w.WriteByte('X');

    MessageReader r(w.data());
    EXPECT_EQ(r.ReadInt32(), 42);
    EXPECT_EQ(r.ReadString(), "test");
    EXPECT_EQ(r.ReadInt16(), 7);
    EXPECT_EQ(r.ReadByte(), 'X');
    EXPECT_TRUE(r.eof());
}

// ===========================================================================
// Part 2: Protocol handler tests — full stack integration
// ===========================================================================

class ProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("protocol_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/mytoydb_protocol_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
    }

    void TearDown() override {
        EndTransactionBlock();
        ResetRelcache();
        ShutdownBufferPool();
        smgrcloseall();
        RunShell("rm -rf " + test_dir_);

        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: commit and start a new transaction so inserted tuples are visible.
    void CommitAndStartNew() {
        EndTransactionBlock();
        InitializeSnapshotManager();
        BeginTransactionBlock();
    }

    // Helper: build a pg_class row and insert it into the catalog.
    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kRelation;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

    // Helper: build a pg_attribute row.
    FormData_pg_attribute* MakeAttrRow(Oid relid, const std::string& name, int16_t attnum,
                                       Oid typid, int16_t attlen, bool attbyval,
                                       AttAlign attalign) {
        auto* row = makePallocNode<FormData_pg_attribute>();
        row->attrelid = relid;
        row->attname = name;
        row->attnum = attnum;
        row->atttypid = typid;
        row->attlen = attlen;
        row->attbyval = attbyval;
        row->attalign = attalign;
        row->attstorage = AttStorage::kPlain;
        return row;
    }

    // Helper: create a relation with the given OID and schema.
    Relation CreateTestRelation(Oid relid, const std::string& name,
                                const std::vector<FormData_pg_attribute>& attrs) {
        auto* class_row = MakeClassRow(name, relid);
        catalog_->InsertClass(class_row);
        for (const auto& attr : attrs) {
            auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
            catalog_->InsertAttribute(attr_row);
        }
        RelationCreateStorage(relid, false);
        return RelationOpen(relid);
    }

    // Helper: build a simple 2-column int4 schema (a, b).
    std::vector<FormData_pg_attribute> MakeIntIntSchema(Oid relid) {
        FormData_pg_attribute a1;
        a1.attrelid = relid;
        a1.attname = "a";
        a1.attnum = 1;
        a1.atttypid = kInt4Oid;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;

        FormData_pg_attribute a2;
        a2.attrelid = relid;
        a2.attname = "b";
        a2.attnum = 2;
        a2.atttypid = kInt4Oid;
        a2.attlen = 4;
        a2.attbyval = true;
        a2.attalign = AttAlign::kInt;
        a2.attstorage = AttStorage::kPlain;

        return {a1, a2};
    }

    // Helper: insert a row (int4, int4) into a relation.
    void InsertIntIntRow(Relation rel, int32_t a, int32_t b) {
        TupleDesc tupdesc = rel->rd_att;
        Datum values[2] = {Int32GetDatum(a), Int32GetDatum(b)};
        bool isnull[2] = {false, false};
        HeapTuple tup = heap_form_tuple(tupdesc, values, isnull);
        heap_insert(rel, tup);
        heap_freetuple(tup);
    }

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// --- Simple Query Protocol ---

TEST_F(ProtocolTest, SimpleQuery_SelectConst) {
    // SELECT 42 — no FROM clause.
    StringSink sink;
    Backend backend(&sink);
    backend.exec_simple_query("SELECT 42;");

    // Expected messages: RowDescription, DataRow, CommandComplete, ReadyForQuery.
    ASSERT_GE(sink.size(), 4u);
    EXPECT_EQ(sink.at(0).type, MessageType::kRowDescription);
    EXPECT_EQ(sink.at(1).type, MessageType::kDataRow);
    EXPECT_EQ(sink.at(2).type, MessageType::kCommandComplete);
    EXPECT_EQ(sink.at(3).type, MessageType::kReadyForQuery);

    // Verify the DataRow contains "42".
    MessageReader r(sink.at(1).payload);
    EXPECT_EQ(r.ReadInt16(), 1);  // 1 column
    int32_t len = r.ReadInt32();
    EXPECT_EQ(len, 2);
    EXPECT_EQ(r.ReadBytes(len), "42");

    // Verify the command tag.
    MessageReader r2(sink.at(2).payload);
    EXPECT_EQ(r2.ReadString(), "SELECT 1");
}

TEST_F(ProtocolTest, SimpleQuery_SelectFromTable) {
    // Create a table with 3 rows.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);
    InsertIntIntRow(rel, 1, 100);
    InsertIntIntRow(rel, 2, 200);
    InsertIntIntRow(rel, 3, 300);
    CommitAndStartNew();
    RelationClose(rel);

    // Query: SELECT * FROM t1
    StringSink sink;
    Backend backend(&sink);
    backend.exec_simple_query("SELECT * FROM t1;");

    // Expected: RowDescription, 3 DataRows, CommandComplete, ReadyForQuery.
    ASSERT_GE(sink.size(), 6u);
    EXPECT_EQ(sink.at(0).type, MessageType::kRowDescription);

    // Verify RowDescription has 2 columns: a, b.
    MessageReader rd(sink.at(0).payload);
    EXPECT_EQ(rd.ReadInt16(), 2);
    // Field 1: name "a"
    EXPECT_EQ(rd.ReadString(), "a");
    rd.ReadInt32();  // table_oid
    rd.ReadInt16();  // column_attnum
    rd.ReadInt32();  // type_oid
    rd.ReadInt16();  // type_size
    rd.ReadInt32();  // type_mod
    rd.ReadInt16();  // format
    // Field 2: name "b"
    EXPECT_EQ(rd.ReadString(), "b");

    // Verify 3 DataRows.
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(sink.at(1 + i).type, MessageType::kDataRow);
        MessageReader r(sink.at(1 + i).payload);
        EXPECT_EQ(r.ReadInt16(), 2);  // 2 columns
        int32_t len1 = r.ReadInt32();
        std::string val1 = r.ReadBytes(len1);
        int32_t len2 = r.ReadInt32();
        std::string val2 = r.ReadBytes(len2);
        EXPECT_EQ(val1, std::to_string(i + 1));
        EXPECT_EQ(val2, std::to_string((i + 1) * 100));
    }

    EXPECT_EQ(sink.at(4).type, MessageType::kCommandComplete);
    MessageReader r2(sink.at(4).payload);
    EXPECT_EQ(r2.ReadString(), "SELECT 3");

    EXPECT_EQ(sink.at(5).type, MessageType::kReadyForQuery);
}

TEST_F(ProtocolTest, SimpleQuery_EmptyQuery) {
    StringSink sink;
    Backend backend(&sink);
    backend.exec_simple_query(";");

    // Expected: EmptyQueryResponse, ReadyForQuery.
    ASSERT_EQ(sink.size(), 2u);
    EXPECT_EQ(sink.at(0).type, MessageType::kEmptyQueryResponse);
    EXPECT_EQ(sink.at(1).type, MessageType::kReadyForQuery);
}

TEST_F(ProtocolTest, SimpleQuery_EmptyString) {
    StringSink sink;
    Backend backend(&sink);
    backend.exec_simple_query("");

    ASSERT_EQ(sink.size(), 2u);
    EXPECT_EQ(sink.at(0).type, MessageType::kEmptyQueryResponse);
    EXPECT_EQ(sink.at(1).type, MessageType::kReadyForQuery);
}

TEST_F(ProtocolTest, SimpleQuery_SyntaxError) {
    StringSink sink;
    Backend backend(&sink);
    backend.exec_simple_query("SELECT FROM;");

    // Expected: ErrorResponse, ReadyForQuery.
    ASSERT_GE(sink.size(), 2u);
    EXPECT_EQ(sink.at(0).type, MessageType::kErrorResponse);
    EXPECT_EQ(sink.at(1).type, MessageType::kReadyForQuery);
}

TEST_F(ProtocolTest, SimpleQuery_Insert) {
    // Create an empty table.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);
    CommitAndStartNew();
    RelationClose(rel);

    // INSERT INTO t1 VALUES (1, 100)
    StringSink sink;
    Backend backend(&sink);
    backend.exec_simple_query("INSERT INTO t1 VALUES (1, 100);");

    // Expected: CommandComplete, ReadyForQuery.
    ASSERT_GE(sink.size(), 2u);
    EXPECT_EQ(sink.at(0).type, MessageType::kCommandComplete);
    MessageReader r(sink.at(0).payload);
    EXPECT_EQ(r.ReadString(), "INSERT 0 1");
    EXPECT_EQ(sink.at(1).type, MessageType::kReadyForQuery);
}

TEST_F(ProtocolTest, SimpleQuery_BeginCommit) {
    StringSink sink;
    Backend backend(&sink);
    backend.exec_simple_query("BEGIN;");

    // BEGIN should produce CommandComplete + ReadyForQuery.
    ASSERT_GE(sink.size(), 2u);
    EXPECT_EQ(sink.at(0).type, MessageType::kCommandComplete);
    MessageReader r(sink.at(0).payload);
    EXPECT_EQ(r.ReadString(), "BEGIN");
    EXPECT_EQ(sink.at(1).type, MessageType::kReadyForQuery);
    // Transaction status should be InTransaction.
    MessageReader r2(sink.at(1).payload);
    EXPECT_EQ(r2.ReadByte(), 'T');  // InTransaction
}

TEST_F(ProtocolTest, SimpleQuery_MultipleStatements) {
    // SELECT 1; SELECT 2; — two statements in one query string.
    StringSink sink;
    Backend backend(&sink);
    backend.exec_simple_query("SELECT 1; SELECT 2;");

    // Expected: (RowDescription, DataRow, CommandComplete) x2, ReadyForQuery.
    ASSERT_GE(sink.size(), 7u);
    // First statement.
    EXPECT_EQ(sink.at(0).type, MessageType::kRowDescription);
    EXPECT_EQ(sink.at(1).type, MessageType::kDataRow);
    EXPECT_EQ(sink.at(2).type, MessageType::kCommandComplete);
    // Second statement.
    EXPECT_EQ(sink.at(3).type, MessageType::kRowDescription);
    EXPECT_EQ(sink.at(4).type, MessageType::kDataRow);
    EXPECT_EQ(sink.at(5).type, MessageType::kCommandComplete);
    // ReadyForQuery.
    EXPECT_EQ(sink.at(6).type, MessageType::kReadyForQuery);

    // Verify first DataRow = "1".
    MessageReader r1(sink.at(1).payload);
    r1.ReadInt16();  // column count
    int32_t len1 = r1.ReadInt32();
    EXPECT_EQ(r1.ReadBytes(len1), "1");

    // Verify second DataRow = "2".
    MessageReader r2(sink.at(4).payload);
    r2.ReadInt16();
    int32_t len2 = r2.ReadInt32();
    EXPECT_EQ(r2.ReadBytes(len2), "2");
}

// --- Extended Query Protocol ---

TEST_F(ProtocolTest, ExtendedQuery_ParseBindExecute) {
    // Parse: SELECT $1
    // Bind: $1 = 42
    // Execute: should return one row with value 42.
    StringSink sink;
    Backend backend(&sink);

    // Parse.
    backend.HandleParse("stmt1", "SELECT $1", {kInt4Oid});
    ASSERT_GE(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kParseComplete);
    sink.clear();

    // Bind.
    backend.HandleBind("portal1", "stmt1", {"42"}, {false});
    ASSERT_GE(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kBindComplete);
    sink.clear();

    // Describe the portal.
    backend.HandleDescribe(DescribeKind::kPortal, "portal1");
    ASSERT_GE(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kRowDescription);
    sink.clear();

    // Execute.
    backend.HandleExecute("portal1", 0);
    ASSERT_GE(sink.size(), 2u);
    EXPECT_EQ(sink.at(0).type, MessageType::kDataRow);
    EXPECT_EQ(sink.at(1).type, MessageType::kCommandComplete);

    // Verify the DataRow value.
    MessageReader r(sink.at(0).payload);
    EXPECT_EQ(r.ReadInt16(), 1);  // 1 column
    int32_t len = r.ReadInt32();
    EXPECT_EQ(r.ReadBytes(len), "42");

    // Verify the command tag.
    MessageReader r2(sink.at(1).payload);
    EXPECT_EQ(r2.ReadString(), "SELECT 1");
}

TEST_F(ProtocolTest, ExtendedQuery_DescribeStatement) {
    // Parse: SELECT $1
    // Describe statement: should get ParameterDescription + RowDescription.
    StringSink sink;
    Backend backend(&sink);

    backend.HandleParse("stmt1", "SELECT $1", {kInt4Oid});
    sink.clear();

    // Describe the statement.
    backend.HandleDescribe(DescribeKind::kStatement, "stmt1");
    ASSERT_GE(sink.size(), 2u);
    // ParameterDescription.
    EXPECT_EQ(sink.at(0).type, MessageType::kParameterDescription);
    MessageReader r0(sink.at(0).payload);
    EXPECT_EQ(r0.ReadInt16(), 1);  // 1 parameter
    EXPECT_EQ(r0.ReadInt32(), static_cast<int32_t>(kInt4Oid));

    // RowDescription.
    EXPECT_EQ(sink.at(1).type, MessageType::kRowDescription);
}

TEST_F(ProtocolTest, ExtendedQuery_Sync) {
    StringSink sink;
    Backend backend(&sink);

    backend.HandleParse("stmt1", "SELECT 42", {});
    sink.clear();

    backend.HandleSync();
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kReadyForQuery);
}

TEST_F(ProtocolTest, ExtendedQuery_CloseStatement) {
    StringSink sink;
    Backend backend(&sink);

    backend.HandleParse("stmt1", "SELECT 42", {});
    ASSERT_NE(backend.FindPreparedStatement("stmt1"), nullptr);
    sink.clear();

    backend.HandleClose(DescribeKind::kStatement, "stmt1");
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kCloseComplete);
    EXPECT_EQ(backend.FindPreparedStatement("stmt1"), nullptr);
}

TEST_F(ProtocolTest, ExtendedQuery_ClosePortal) {
    StringSink sink;
    Backend backend(&sink);

    backend.HandleParse("stmt1", "SELECT 42", {});
    backend.HandleBind("portal1", "stmt1", {}, {});
    ASSERT_NE(backend.FindPortal("portal1"), nullptr);
    sink.clear();

    backend.HandleClose(DescribeKind::kPortal, "portal1");
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kCloseComplete);
    EXPECT_EQ(backend.FindPortal("portal1"), nullptr);
}

TEST_F(ProtocolTest, ExtendedQuery_BindMissingStatement) {
    StringSink sink;
    Backend backend(&sink);

    // Try to bind to a non-existent statement.
    backend.HandleBind("portal1", "nonexistent", {}, {});
    ASSERT_GE(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kErrorResponse);
}

TEST_F(ProtocolTest, ExtendedQuery_ExecuteMissingPortal) {
    StringSink sink;
    Backend backend(&sink);

    backend.HandleExecute("nonexistent", 0);
    ASSERT_GE(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kErrorResponse);
}

TEST_F(ProtocolTest, ExtendedQuery_UnnamedStatement) {
    // Use the unnamed prepared statement (empty name).
    StringSink sink;
    Backend backend(&sink);

    backend.HandleParse("", "SELECT 42", {});
    ASSERT_NE(backend.FindPreparedStatement(""), nullptr);
    sink.clear();

    backend.HandleBind("", "", {}, {});
    ASSERT_NE(backend.FindPortal(""), nullptr);
    sink.clear();

    backend.HandleExecute("", 0);
    ASSERT_GE(sink.size(), 2u);
    EXPECT_EQ(sink.at(0).type, MessageType::kDataRow);
    EXPECT_EQ(sink.at(1).type, MessageType::kCommandComplete);

    MessageReader r(sink.at(0).payload);
    r.ReadInt16();  // column count
    int32_t len = r.ReadInt32();
    EXPECT_EQ(r.ReadBytes(len), "42");
}

TEST_F(ProtocolTest, ExtendedQuery_SelectFromTable) {
    // Create a table with 2 rows.
    Oid relid = kFirstNormalObjectId;
    auto attrs = MakeIntIntSchema(relid);
    Relation rel = CreateTestRelation(relid, "t1", attrs);
    InsertIntIntRow(rel, 10, 20);
    InsertIntIntRow(rel, 30, 40);
    CommitAndStartNew();
    RelationClose(rel);

    StringSink sink;
    Backend backend(&sink);

    // Parse + Bind + Execute: SELECT * FROM t1
    backend.HandleParse("stmt1", "SELECT * FROM t1", {});
    backend.HandleBind("portal1", "stmt1", {}, {});
    sink.clear();

    backend.HandleExecute("portal1", 0);

    // Expected: 2 DataRows + CommandComplete.
    ASSERT_GE(sink.size(), 3u);
    EXPECT_EQ(sink.at(0).type, MessageType::kDataRow);
    EXPECT_EQ(sink.at(1).type, MessageType::kDataRow);
    EXPECT_EQ(sink.at(2).type, MessageType::kCommandComplete);

    // Verify first row.
    MessageReader r1(sink.at(0).payload);
    r1.ReadInt16();  // column count
    int32_t len1a = r1.ReadInt32();
    EXPECT_EQ(r1.ReadBytes(len1a), "10");
    int32_t len1b = r1.ReadInt32();
    EXPECT_EQ(r1.ReadBytes(len1b), "20");

    // Verify second row.
    MessageReader r2(sink.at(1).payload);
    r2.ReadInt16();
    int32_t len2a = r2.ReadInt32();
    EXPECT_EQ(r2.ReadBytes(len2a), "30");
    int32_t len2b = r2.ReadInt32();
    EXPECT_EQ(r2.ReadBytes(len2b), "40");

    // Verify command tag.
    MessageReader r3(sink.at(2).payload);
    EXPECT_EQ(r3.ReadString(), "SELECT 2");
}

}  // namespace
