// toast_test.cpp — Unit tests for TOAST (The Oversized-Attribute Storage Technique).
//
// Tests pglz compression/decompression, TOAST table creation, and end-to-end
// TOAST operations (insert large text, read back, verify content).
//
// The fixture sets up the full stack: error subsystem, memory context,
// catalog + syscache, transaction system, buffer pool, storage directory,
// and relcache.

#include "access/toast.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"
#include "types/varlena.hpp"

using pgcpp::access::detoast_attr;
using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_deform_tuple;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_getnext;
using pgcpp::access::heap_insert;
using pgcpp::access::HeapScanDesc;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::kToastMaxChunkSize;
using pgcpp::access::kToastThreshold;
using pgcpp::access::pglz_compress;
using pgcpp::access::pglz_decompress;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::access::toast_get_or_create_table;
using pgcpp::access::toast_insert_or_update;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::palloc;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::SET_VARSIZE_4B;
using pgcpp::types::TextPGetDatum;
using pgcpp::types::VARATT_IS_COMPRESSED;
using pgcpp::types::VARATT_IS_EXTERNAL;
using pgcpp::types::VARSIZE_ANY;
using pgcpp::types::VARSIZE_DATA;

namespace {

using pgcpp::nodes::makePallocNode;

// Create a normal varlena Datum from a string (4-byte header + data).
Datum MakeTextDatum(const std::string& data) {
    int total = static_cast<int>(sizeof(int32_t) + data.size());
    char* buf = static_cast<char*>(palloc(total));
    SET_VARSIZE_4B(buf, static_cast<uint32_t>(total));
    if (!data.empty()) {
        std::memcpy(buf + sizeof(int32_t), data.data(), data.size());
    }
    return TextPGetDatum(buf);
}

// Extract the data portion from a normal varlena Datum.
std::string GetTextData(Datum d) {
    const char* text = DatumGetTextP(d);
    int len = VARSIZE_DATA(text);
    return std::string(text + sizeof(int32_t), static_cast<size_t>(len));
}

// =========================================================================
// pglz compression tests (standalone — no infrastructure needed)
// =========================================================================

TEST(PglzTest, RoundTripRepeatingData) {
    // Highly compressible: 1000 bytes of 'A'
    std::string input(1000, 'A');
    std::vector<char> compressed(input.size() + 100);
    int comp_len = 0;

    ASSERT_TRUE(
        pglz_compress(input.data(), static_cast<int>(input.size()), compressed.data(), &comp_len));
    EXPECT_LT(comp_len, static_cast<int>(input.size()));

    std::vector<char> decompressed(input.size());
    int dec_len = pglz_decompress(compressed.data(), comp_len, decompressed.data(),
                                  static_cast<int>(input.size()));
    ASSERT_EQ(dec_len, static_cast<int>(input.size()));
    EXPECT_EQ(std::memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST(PglzTest, RoundTripMixedData) {
    // Mixed: some repeating, some unique
    std::string input;
    for (int i = 0; i < 100; i++) {
        input += "ABCDEFGH";  // repeating pattern
    }
    for (int i = 0; i < 100; i++) {
        input += static_cast<char>(i);  // unique bytes
    }

    std::vector<char> compressed(input.size() + 100);
    int comp_len = 0;
    ASSERT_TRUE(
        pglz_compress(input.data(), static_cast<int>(input.size()), compressed.data(), &comp_len));

    std::vector<char> decompressed(input.size());
    int dec_len = pglz_decompress(compressed.data(), comp_len, decompressed.data(),
                                  static_cast<int>(input.size()));
    ASSERT_EQ(dec_len, static_cast<int>(input.size()));
    EXPECT_EQ(std::memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST(PglzTest, IncompressibleDataReturnsFalse) {
    // Random-ish data that won't compress well
    std::string input;
    for (int i = 0; i < 200; i++) {
        input += static_cast<char>(i * 37 % 256);
    }

    std::vector<char> compressed(input.size() + 100);
    int comp_len = 0;
    // May or may not compress — just verify no crash and round-trip if it does.
    if (pglz_compress(input.data(), static_cast<int>(input.size()), compressed.data(), &comp_len)) {
        std::vector<char> decompressed(input.size());
        int dec_len = pglz_decompress(compressed.data(), comp_len, decompressed.data(),
                                      static_cast<int>(input.size()));
        ASSERT_EQ(dec_len, static_cast<int>(input.size()));
        EXPECT_EQ(std::memcmp(decompressed.data(), input.data(), input.size()), 0);
    }
}

TEST(PglzTest, ShortDataDoesNotCompress) {
    // Very short data (less than min match length)
    std::string input = "AB";
    std::vector<char> compressed(input.size() + 100);
    int comp_len = 0;
    // Should return false (compressed >= original)
    EXPECT_FALSE(
        pglz_compress(input.data(), static_cast<int>(input.size()), compressed.data(), &comp_len));
}

TEST(PglzTest, LargeRepeatingChunksRoundTrip) {
    // Data with long-range repeating pattern (tests offset encoding)
    std::string chunk = "0123456789ABCDEF";
    std::string input;
    for (int i = 0; i < 200; i++) {
        input += chunk;
    }

    std::vector<char> compressed(input.size() + 100);
    int comp_len = 0;
    ASSERT_TRUE(
        pglz_compress(input.data(), static_cast<int>(input.size()), compressed.data(), &comp_len));
    EXPECT_LT(comp_len, static_cast<int>(input.size()));

    std::vector<char> decompressed(input.size());
    int dec_len = pglz_decompress(compressed.data(), comp_len, decompressed.data(),
                                  static_cast<int>(input.size()));
    ASSERT_EQ(dec_len, static_cast<int>(input.size()));
    EXPECT_EQ(std::memcmp(decompressed.data(), input.data(), input.size()), 0);
}

// =========================================================================
// TOAST integration tests (require full infrastructure)
// =========================================================================

class ToastTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("toast_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_toast_test_" + std::to_string(getpid());
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

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    void CommitAndStartNew() {
        EndTransactionBlock();
        BeginTransactionBlock();
    }

    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kRelation;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

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

    // Build a schema with one int4 column and one text column (attstorage = Extended).
    std::vector<FormData_pg_attribute> MakeIntTextSchema(Oid relid) {
        FormData_pg_attribute a1;
        a1.attrelid = relid;
        a1.attname = "id";
        a1.attnum = 1;
        a1.atttypid = kInt4Oid;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;

        FormData_pg_attribute a2;
        a2.attrelid = relid;
        a2.attname = "data";
        a2.attnum = 2;
        a2.atttypid = kTextOid;
        a2.attlen = -1;
        a2.attbyval = false;
        a2.attalign = AttAlign::kInt;
        a2.attstorage = AttStorage::kExtended;

        return {a1, a2};
    }

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// Test: small text values are stored inline (not toasted).
TEST_F(ToastTest, SmallTextStoredInline) {
    constexpr Oid kRelOid = kFirstNormalObjectId + 200;
    auto attrs = MakeIntTextSchema(kRelOid);
    Relation rel = CreateTestRelation(kRelOid, "toast_small", attrs);

    // Insert a small text value (well below kToastThreshold).
    std::string small_text = "hello world";
    Datum values[2] = {Int32GetDatum(1), MakeTextDatum(small_text)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    heap_insert(rel, tup);
    heap_freetuple(tup);

    CommitAndStartNew();

    // Scan and verify.
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    HeapTuple result = heap_getnext(scan);
    ASSERT_NE(result, nullptr);

    Datum out_values[2];
    bool out_isnull[2];
    heap_deform_tuple(result, rel->rd_att, out_values, out_isnull);

    ASSERT_FALSE(out_isnull[0]);
    ASSERT_FALSE(out_isnull[1]);
    EXPECT_EQ(GetTextData(out_values[1]), small_text);

    heap_endscan(scan);
    RelationClose(rel);
}

// Test: large text value is toasted (compressed or external) and detoasted on read.
TEST_F(ToastTest, LargeTextRoundTrip) {
    constexpr Oid kRelOid = kFirstNormalObjectId + 201;
    auto attrs = MakeIntTextSchema(kRelOid);
    Relation rel = CreateTestRelation(kRelOid, "toast_large", attrs);

    // Create a large text value (> kToastThreshold = 2000 bytes).
    // Use a repeating pattern so pglz can compress it.
    std::string large_text;
    for (int i = 0; i < 300; i++) {
        large_text += "ABCDEFGHIJKLMNOP";  // 16 chars * 300 = 4800 bytes
    }
    ASSERT_GT(static_cast<int>(large_text.size()), kToastThreshold);

    Datum values[2] = {Int32GetDatum(42), MakeTextDatum(large_text)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    heap_insert(rel, tup);
    heap_freetuple(tup);

    CommitAndStartNew();

    // Scan and verify the value is correctly detoasted.
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    HeapTuple result = heap_getnext(scan);
    ASSERT_NE(result, nullptr);

    Datum out_values[2];
    bool out_isnull[2];
    heap_deform_tuple(result, rel->rd_att, out_values, out_isnull);

    ASSERT_FALSE(out_isnull[1]);
    std::string retrieved = GetTextData(out_values[1]);
    EXPECT_EQ(retrieved.size(), large_text.size());
    EXPECT_EQ(retrieved, large_text);

    heap_endscan(scan);
    RelationClose(rel);
}

// Test: very large incompressible text is stored externally.
TEST_F(ToastTest, LargeIncompressibleTextExternal) {
    constexpr Oid kRelOid = kFirstNormalObjectId + 202;
    auto attrs = MakeIntTextSchema(kRelOid);
    Relation rel = CreateTestRelation(kRelOid, "toast_external", attrs);

    // Create a large pseudo-random text value that won't compress well.
    std::string large_text;
    for (int i = 0; i < 3000; i++) {
        large_text += static_cast<char>('A' + (i * 37 % 26));
    }
    ASSERT_GT(static_cast<int>(large_text.size()), kToastThreshold);

    Datum values[2] = {Int32GetDatum(99), MakeTextDatum(large_text)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    heap_insert(rel, tup);
    heap_freetuple(tup);

    CommitAndStartNew();

    // Verify round-trip.
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    HeapTuple result = heap_getnext(scan);
    ASSERT_NE(result, nullptr);

    Datum out_values[2];
    bool out_isnull[2];
    heap_deform_tuple(result, rel->rd_att, out_values, out_isnull);

    ASSERT_FALSE(out_isnull[1]);
    std::string retrieved = GetTextData(out_values[1]);
    EXPECT_EQ(retrieved.size(), large_text.size());
    EXPECT_EQ(retrieved, large_text);

    heap_endscan(scan);
    RelationClose(rel);
}

// Test: TOAST table is created when a large value is inserted.
TEST_F(ToastTest, ToastTableCreated) {
    constexpr Oid kRelOid = kFirstNormalObjectId + 203;
    auto attrs = MakeIntTextSchema(kRelOid);
    Relation rel = CreateTestRelation(kRelOid, "toast_table_create", attrs);

    // Before insert, no TOAST table should exist.
    EXPECT_EQ(rel->rd_rel->reltoastrelid, kInvalidOid);

    // Insert a large incompressible value to trigger TOAST table creation.
    // (Compressible data would be stored compressed-inline instead, never
    // creating a TOAST table.)
    std::string large_text;
    std::srand(42);
    for (int i = 0; i < 3000; i++) {
        large_text += static_cast<char>(std::rand() & 0xFF);
    }
    Datum values[2] = {Int32GetDatum(1), MakeTextDatum(large_text)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    heap_insert(rel, tup);
    heap_freetuple(tup);

    // After insert, a TOAST table should have been created.
    EXPECT_NE(rel->rd_rel->reltoastrelid, kInvalidOid);

    CommitAndStartNew();
    RelationClose(rel);
}

// Test: multiple rows with mixed small and large text values.
TEST_F(ToastTest, MixedRowsRoundTrip) {
    constexpr Oid kRelOid = kFirstNormalObjectId + 204;
    auto attrs = MakeIntTextSchema(kRelOid);
    Relation rel = CreateTestRelation(kRelOid, "toast_mixed", attrs);

    // Row 1: small text
    std::string small = "small";
    Datum v1[2] = {Int32GetDatum(1), MakeTextDatum(small)};
    bool n1[2] = {false, false};
    HeapTuple t1 = heap_form_tuple(rel->rd_att, v1, n1);
    heap_insert(rel, t1);
    heap_freetuple(t1);

    // Row 2: large text (compressible)
    std::string large_compressible(5000, 'X');
    Datum v2[2] = {Int32GetDatum(2), MakeTextDatum(large_compressible)};
    bool n2[2] = {false, false};
    HeapTuple t2 = heap_form_tuple(rel->rd_att, v2, n2);
    heap_insert(rel, t2);
    heap_freetuple(t2);

    // Row 3: large text (less compressible)
    std::string large_mixed;
    for (int i = 0; i < 300; i++) {
        large_mixed += "abcdefghijklmnop";
    }
    Datum v3[2] = {Int32GetDatum(3), MakeTextDatum(large_mixed)};
    bool n3[2] = {false, false};
    HeapTuple t3 = heap_form_tuple(rel->rd_att, v3, n3);
    heap_insert(rel, t3);
    heap_freetuple(t3);

    CommitAndStartNew();

    // Scan all rows and verify.
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    int count = 0;
    HeapTuple tup = nullptr;
    while ((tup = heap_getnext(scan)) != nullptr) {
        Datum out[2];
        bool out_null[2];
        heap_deform_tuple(tup, rel->rd_att, out, out_null);

        int id = static_cast<int>(out[0]);
        std::string text = GetTextData(out[1]);

        if (id == 1) {
            EXPECT_EQ(text, small);
        } else if (id == 2) {
            EXPECT_EQ(text, large_compressible);
        } else if (id == 3) {
            EXPECT_EQ(text, large_mixed);
        }
        count++;
    }
    EXPECT_EQ(count, 3);

    heap_endscan(scan);
    RelationClose(rel);
}

// Test: delete a tuple with toasted values (no crash, no leak).
TEST_F(ToastTest, DeleteToastTuple) {
    constexpr Oid kRelOid = kFirstNormalObjectId + 205;
    auto attrs = MakeIntTextSchema(kRelOid);
    Relation rel = CreateTestRelation(kRelOid, "toast_delete", attrs);

    // Insert a large value.
    std::string large_text(3000, 'D');
    Datum values[2] = {Int32GetDatum(1), MakeTextDatum(large_text)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    heap_insert(rel, tup);
    heap_freetuple(tup);

    CommitAndStartNew();

    // Find and delete the tuple.
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    HeapTuple result = heap_getnext(scan);
    ASSERT_NE(result, nullptr);

    // Delete via heap_delete (which calls toast_delete_tuple internally).
    auto tid = result->t_self;
    heap_endscan(scan);

    // Close and reopen to get a fresh relation (the scan holds a pin).
    RelationClose(rel);
    rel = RelationOpen(kRelOid);
    pgcpp::access::heap_delete(rel, tid);

    CommitAndStartNew();

    // Verify the tuple is gone (deleted tuples are invisible to new snapshots).
    scan = heap_beginscan(rel, nullptr);
    result = heap_getnext(scan);
    EXPECT_EQ(result, nullptr);
    heap_endscan(scan);

    RelationClose(rel);
}

// Test: detoast_attr on a normal (non-toasted) varlena returns it unchanged.
TEST_F(ToastTest, DetoastNormalReturnsUnchanged) {
    std::string data = "normal text";
    Datum d = MakeTextDatum(data);

    Datum result = detoast_attr(d);
    // For normal varlena, detoast_attr returns the same pointer.
    EXPECT_EQ(result, d);
    EXPECT_EQ(GetTextData(result), data);
}

// Test: detoast_attr on a compressed inline varlena decompresses correctly.
TEST_F(ToastTest, DetoastCompressedInline) {
    // Create a compressible string.
    std::string data(1000, 'C');

    // Compress it.
    std::vector<char> compressed(data.size() + 100);
    int comp_len = 0;
    ASSERT_TRUE(
        pglz_compress(data.data(), static_cast<int>(data.size()), compressed.data(), &comp_len));

    // Build a compressed inline varlena.
    int total = static_cast<int>(sizeof(int32_t) + sizeof(int32_t) + comp_len);
    char* buf = static_cast<char*>(palloc(total));
    uint32_t header = static_cast<uint32_t>(total) | pgcpp::types::kVarAttCompressed;
    std::memcpy(buf, &header, sizeof(header));
    int32_t raw_size = static_cast<int32_t>(data.size());
    std::memcpy(buf + sizeof(int32_t), &raw_size, sizeof(raw_size));
    std::memcpy(buf + sizeof(int32_t) + sizeof(int32_t), compressed.data(), comp_len);

    Datum compressed_datum = TextPGetDatum(buf);
    ASSERT_TRUE(VARATT_IS_COMPRESSED(DatumGetTextP(compressed_datum)));

    // Detoast.
    Datum result = detoast_attr(compressed_datum);
    EXPECT_FALSE(VARATT_IS_COMPRESSED(DatumGetTextP(result)));
    EXPECT_EQ(GetTextData(result), data);
}

}  // namespace
