// dest_test.cpp — Unit tests for DestReceiver (M11 Phase 15.9).
//
// Tests the DestReceiver abstraction and its concrete subclasses:
//   NoneReceiver, DebugReceiver, RemoteReceiver, TuplestoreReceiver,
//   IntoRelReceiver. Also tests the factory, EncodeDatumAsText, and
//   GetTuplestoreSlots helpers.

#include "protocol/dest.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/tupletable.hpp"
#include "protocol/pqformat.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::CreateTupleDesc;
using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_deform_tuple;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_getnext;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::protocol::CommandDest;
using pgcpp::protocol::CreateDestReceiver;
using pgcpp::protocol::CreateIntoRelReceiver;
using pgcpp::protocol::CreateNoneReceiver;
using pgcpp::protocol::CreateRemoteReceiver;
using pgcpp::protocol::CreateTuplestoreReceiver;
using pgcpp::protocol::DestReceiver;
using pgcpp::protocol::EncodeDatumAsText;
using pgcpp::protocol::GetTuplestoreSlots;
using pgcpp::protocol::MessageReader;
using pgcpp::protocol::MessageType;
using pgcpp::protocol::StringSink;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::GetTransactionSnapshot;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::BoolGetDatum;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

class DestTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("dest_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_dest_test_" + std::to_string(getpid());
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

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    void CommitAndStartNew() {
        EndTransactionBlock();
        InitializeSnapshotManager();
        BeginTransactionBlock();
    }

    // Create a relation with OID and an int4 column "a".
    Relation CreateIntTable(Oid relid, const std::string& name) {
        auto* class_row = makePallocNode<FormData_pg_class>();
        class_row->oid = relid;
        class_row->relname = name;
        class_row->relfilenode = relid;
        class_row->relkind = RelKind::kRelation;
        class_row->relpersistence = RelPersistence::kPermanent;
        class_row->relnatts = 1;
        class_row->relispopulated = true;
        catalog_->InsertClass(class_row);

        auto* attr = makePallocNode<FormData_pg_attribute>();
        attr->attrelid = relid;
        attr->attname = "a";
        attr->attnum = 1;
        attr->atttypid = kInt4Oid;
        attr->attlen = 4;
        attr->attbyval = true;
        attr->attalign = AttAlign::kInt;
        attr->attstorage = AttStorage::kPlain;
        catalog_->InsertAttribute(attr);

        RelationCreateStorage(relid, false);
        return RelationOpen(relid);
    }

    TupleDesc MakeIntTupdesc() {
        FormData_pg_attribute a;
        a.attrelid = 0;
        a.attname = "a";
        a.attnum = 1;
        a.atttypid = kInt4Oid;
        a.attlen = 4;
        a.attbyval = true;
        a.attalign = AttAlign::kInt;
        a.attstorage = AttStorage::kPlain;
        return CreateTupleDesc({a});
    }

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// --- Factory tests ---

TEST_F(DestTest, Factory_NoneReturnsNoneDest) {
    DestReceiver* r = CreateDestReceiver(CommandDest::kNone, nullptr);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->mydest, CommandDest::kNone);
    r->rDestroy();
    delete r;
}

TEST_F(DestTest, Factory_DebugReturnsDebugDest) {
    DestReceiver* r = CreateDestReceiver(CommandDest::kDebug, nullptr);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->mydest, CommandDest::kDebug);
    r->rDestroy();
    delete r;
}

TEST_F(DestTest, Factory_RemoteReturnsRemoteDest) {
    StringSink sink;
    DestReceiver* r = CreateDestReceiver(CommandDest::kRemote, &sink);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->mydest, CommandDest::kRemote);
    r->rDestroy();
    delete r;
}

TEST_F(DestTest, Factory_RemoteExecuteReturnsRemoteExecuteDest) {
    StringSink sink;
    DestReceiver* r = CreateDestReceiver(CommandDest::kRemoteExecute, &sink);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->mydest, CommandDest::kRemoteExecute);
    r->rDestroy();
    delete r;
}

TEST_F(DestTest, Factory_TuplestoreReturnsTuplestoreDest) {
    DestReceiver* r = CreateDestReceiver(CommandDest::kTuplestore, nullptr);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->mydest, CommandDest::kTuplestore);
    r->rDestroy();
    delete r;
}

// --- NoneReceiver ---

TEST_F(DestTest, NoneReceiver_DiscardsSlots) {
    DestReceiver* r = CreateNoneReceiver();
    TupleDesc tupdesc = MakeIntTupdesc();
    r->rStartup(nullptr, 0, tupdesc);

    auto* slot = TupleTableSlot::Make(tupdesc);
    Datum val = Int32GetDatum(42);
    bool isnull = false;
    slot->StoreVirtual(&val, &isnull);
    EXPECT_TRUE(r->receiveSlot(slot, nullptr));
    r->rShutdown(nullptr);
    r->rDestroy();
    delete r;
    // NOTE: `slot` is allocated via TupleTableSlot::Make() which uses
    // makePallocNode (palloc). It is freed when the per-test memory
    // context is destroyed in TearDown; calling `delete` on it would
    // be a bad-free (detected by ASan).
}

// --- RemoteReceiver ---

TEST_F(DestTest, RemoteReceiver_SendsRowDescriptionAndDataRow) {
    StringSink sink;
    DestReceiver* r = CreateRemoteReceiver(&sink, /*send_row_description=*/true);
    TupleDesc tupdesc = MakeIntTupdesc();
    r->rStartup(nullptr, 0, tupdesc);

    // rStartup should send a RowDescription.
    ASSERT_GE(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kRowDescription);
    MessageReader rd(sink.at(0).payload);
    EXPECT_EQ(rd.ReadInt16(), 1);  // 1 column
    EXPECT_EQ(rd.ReadString(), "a");

    // receiveSlot should send a DataRow.
    auto* slot = TupleTableSlot::Make(tupdesc);
    Datum val = Int32GetDatum(7);
    bool isnull = false;
    slot->StoreVirtual(&val, &isnull);
    EXPECT_TRUE(r->receiveSlot(slot, nullptr));

    ASSERT_GE(sink.size(), 2u);
    EXPECT_EQ(sink.at(1).type, MessageType::kDataRow);
    MessageReader dr(sink.at(1).payload);
    EXPECT_EQ(dr.ReadInt16(), 1);  // 1 column
    int32_t len = dr.ReadInt32();
    EXPECT_EQ(dr.ReadBytes(len), "7");

    r->rShutdown(nullptr);
    r->rDestroy();
    delete r;
    // NOTE: `slot` is allocated via TupleTableSlot::Make() which uses
    // makePallocNode (palloc). It is freed when the per-test memory
    // context is destroyed in TearDown; calling `delete` on it would
    // be a bad-free (detected by ASan).
}

TEST_F(DestTest, RemoteReceiver_NoRowDescriptionWhenDisabled) {
    StringSink sink;
    DestReceiver* r = CreateRemoteReceiver(&sink, /*send_row_description=*/false);
    TupleDesc tupdesc = MakeIntTupdesc();
    r->rStartup(nullptr, 0, tupdesc);

    // No RowDescription should be sent.
    EXPECT_EQ(sink.size(), 0u);

    auto* slot = TupleTableSlot::Make(tupdesc);
    Datum val = Int32GetDatum(99);
    bool isnull = false;
    slot->StoreVirtual(&val, &isnull);
    EXPECT_TRUE(r->receiveSlot(slot, nullptr));

    // Only the DataRow should be present.
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink.at(0).type, MessageType::kDataRow);

    r->rDestroy();
    delete r;
    // NOTE: `slot` is allocated via TupleTableSlot::Make() which uses
    // makePallocNode (palloc). It is freed when the per-test memory
    // context is destroyed in TearDown; calling `delete` on it would
    // be a bad-free (detected by ASan).
}

// --- TuplestoreReceiver ---

TEST_F(DestTest, TuplestoreReceiver_CollectsSlots) {
    DestReceiver* r = CreateTuplestoreReceiver();
    TupleDesc tupdesc = MakeIntTupdesc();
    r->rStartup(nullptr, 0, tupdesc);

    auto* slot = TupleTableSlot::Make(tupdesc);
    for (int i = 0; i < 3; ++i) {
        Datum val = Int32GetDatum(i + 10);
        bool isnull = false;
        slot->StoreVirtual(&val, &isnull);
        EXPECT_TRUE(r->receiveSlot(slot, nullptr));
    }

    auto collected = GetTuplestoreSlots(r);
    ASSERT_EQ(collected.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        ASSERT_NE(collected[i], nullptr);
        EXPECT_FALSE(collected[i]->tts_isnull[0]);
        EXPECT_EQ(DatumGetInt32(collected[i]->tts_values[0]), i + 10);
    }

    r->rDestroy();
    delete r;
    // NOTE: `slot` is allocated via TupleTableSlot::Make() which uses
    // makePallocNode (palloc). It is freed when the per-test memory
    // context is destroyed in TearDown; calling `delete` on it would
    // be a bad-free (detected by ASan).
}

TEST_F(DestTest, GetTuplestoreSlots_EmptyForNonTuplestoreReceiver) {
    DestReceiver* r = CreateNoneReceiver();
    auto collected = GetTuplestoreSlots(r);
    EXPECT_TRUE(collected.empty());
    r->rDestroy();
    delete r;
}

// --- IntoRelReceiver ---

TEST_F(DestTest, IntoRelReceiver_InsertsTuplesIntoRelation) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateIntTable(relid, "dst");
    CommitAndStartNew();
    RelationClose(rel);

    DestReceiver* r = CreateIntoRelReceiver(relid);
    TupleDesc tupdesc = MakeIntTupdesc();
    r->rStartup(nullptr, 0, tupdesc);

    auto* slot = TupleTableSlot::Make(tupdesc);
    for (int i = 0; i < 2; ++i) {
        Datum val = Int32GetDatum(i + 100);
        bool isnull = false;
        slot->StoreVirtual(&val, &isnull);
        EXPECT_TRUE(r->receiveSlot(slot, nullptr));
    }
    r->rShutdown(nullptr);
    r->rDestroy();
    delete r;
    // NOTE: `slot` is allocated via TupleTableSlot::Make() which uses
    // makePallocNode (palloc). It is freed when the per-test memory
    // context is destroyed in TearDown; calling `delete` on it would
    // be a bad-free (detected by ASan).

    // Verify the tuples were inserted by scanning the relation.
    CommitAndStartNew();
    rel = RelationOpen(relid);
    ASSERT_NE(rel, nullptr);
    auto* scan = heap_beginscan(rel, GetTransactionSnapshot());
    int count = 0;
    HeapTuple tup;
    while ((tup = heap_getnext(scan)) != nullptr) {
        Datum values[1];
        bool isnull[1];
        heap_deform_tuple(tup, rel->rd_att, values, isnull);
        EXPECT_FALSE(isnull[0]);
        EXPECT_EQ(DatumGetInt32(values[0]), 100 + count);
        ++count;
    }
    heap_endscan(scan);
    RelationClose(rel);
    EXPECT_EQ(count, 2);
}

// --- EncodeDatumAsText ---

TEST_F(DestTest, EncodeDatumAsText_Int4) {
    Datum v = Int32GetDatum(12345);
    EXPECT_EQ(EncodeDatumAsText(v, kInt4Oid), "12345");
}

TEST_F(DestTest, EncodeDatumAsText_Int4_Negative) {
    Datum v = Int32GetDatum(-1);
    EXPECT_EQ(EncodeDatumAsText(v, kInt4Oid), "-1");
}

TEST_F(DestTest, EncodeDatumAsText_BoolTrue) {
    Datum v = BoolGetDatum(true);
    EXPECT_EQ(EncodeDatumAsText(v, kBoolOid), "t");
}

TEST_F(DestTest, EncodeDatumAsText_BoolFalse) {
    Datum v = BoolGetDatum(false);
    EXPECT_EQ(EncodeDatumAsText(v, kBoolOid), "f");
}

}  // namespace
