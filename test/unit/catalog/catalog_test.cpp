#include "catalog/catalog.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_type.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::CatalogTupleDelete;
using pgcpp::catalog::CatalogTupleInsert;
using pgcpp::catalog::CatalogTupleUpdate;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::GetSysCache;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::ReleaseSysCache;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SearchSysCache1;
using pgcpp::catalog::SearchSysCache2;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::catalog::SysCacheIdentifier;
using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;

class CatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("catalog_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);

        syscache_ = new SysCache();
        SetSysCache(syscache_);
    }

    void TearDown() override {
        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: allocate a pg_class row in the current memory context.
    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid = kInvalidOid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->relname = name;
        if (oid != kInvalidOid) {
            row->oid = oid;
        }
        return row;
    }

    FormData_pg_attribute* MakeAttributeRow(Oid relid, const std::string& name, int16_t attnum,
                                            Oid typid) {
        auto* row = makePallocNode<FormData_pg_attribute>();
        row->attrelid = relid;
        row->attname = name;
        row->attnum = attnum;
        row->atttypid = typid;
        return row;
    }

    FormData_pg_type* MakeTypeRow(const std::string& name, Oid oid = kInvalidOid) {
        auto* row = makePallocNode<FormData_pg_type>();
        row->typname = name;
        if (oid != kInvalidOid) {
            row->oid = oid;
        }
        return row;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// Returns true if the given callable ereports(ERROR).
template<typename F>
bool RaisesError(F&& fn) {
    bool caught = false;
    PG_TRY() {
        fn();
    }
    PG_CATCH() {
        caught = true;
        ErrorData* err = pgcpp::error::GetErrorData();
        EXPECT_EQ(err->elevel, LogLevel::kError);
    }
    PG_END_TRY();
    return caught;
}

// ===========================================================================
// Oid / constants
// ===========================================================================

TEST_F(CatalogTest, OidIsUint32) {
    static_assert(std::is_same_v<Oid, uint32_t>, "Oid must be uint32_t");
}

TEST_F(CatalogTest, InvalidOidIsZero) {
    EXPECT_EQ(kInvalidOid, 0u);
}

TEST_F(CatalogTest, FirstNormalObjectIdIs16384) {
    EXPECT_EQ(kFirstNormalObjectId, 16384u);
}

// ===========================================================================
// pg_class struct layout / defaults
// ===========================================================================

TEST_F(CatalogTest, PgClassDefaults) {
    FormData_pg_class row;
    EXPECT_EQ(row.oid, kInvalidOid);
    EXPECT_TRUE(row.relname.empty());
    EXPECT_EQ(row.relkind, RelKind::kRelation);
    EXPECT_EQ(row.relpersistence, RelPersistence::kPermanent);
    EXPECT_EQ(row.relnatts, 0);
    EXPECT_FALSE(row.relhasindex);
    EXPECT_FALSE(row.relisshared);
    EXPECT_TRUE(row.relispopulated);
}

TEST_F(CatalogTest, PgClassFieldAssignment) {
    FormData_pg_class row;
    row.oid = 16384;
    row.relname = "hits";
    row.relnatts = 5;
    row.relkind = RelKind::kRelation;
    row.relpersistence = RelPersistence::kPermanent;
    EXPECT_EQ(row.oid, 16384u);
    EXPECT_EQ(row.relname, "hits");
    EXPECT_EQ(row.relnatts, 5);
}

// ===========================================================================
// pg_attribute struct layout / defaults
// ===========================================================================

TEST_F(CatalogTest, PgAttributeDefaults) {
    FormData_pg_attribute row;
    EXPECT_EQ(row.attrelid, kInvalidOid);
    EXPECT_TRUE(row.attname.empty());
    EXPECT_EQ(row.attnum, 0);
    EXPECT_EQ(row.atttypid, kInvalidOid);
    EXPECT_EQ(row.atttypmod, -1);
    EXPECT_FALSE(row.attnotnull);
    EXPECT_FALSE(row.atthasdef);
    EXPECT_FALSE(row.attisdropped);
    EXPECT_TRUE(row.attislocal);
}

TEST_F(CatalogTest, PgAttributeFieldAssignment) {
    FormData_pg_attribute row;
    row.attrelid = 16384;
    row.attname = "id";
    row.attnum = 1;
    row.atttypid = 23;  // int4
    row.attlen = 4;
    row.attbyval = true;
    row.attnotnull = true;
    EXPECT_EQ(row.attrelid, 16384u);
    EXPECT_EQ(row.attname, "id");
    EXPECT_EQ(row.attnum, 1);
    EXPECT_EQ(row.atttypid, 23u);
    EXPECT_TRUE(row.attbyval);
    EXPECT_TRUE(row.attnotnull);
}

// ===========================================================================
// pg_type struct layout / defaults
// ===========================================================================

TEST_F(CatalogTest, PgTypeDefaults) {
    FormData_pg_type row;
    EXPECT_EQ(row.oid, kInvalidOid);
    EXPECT_TRUE(row.typname.empty());
    EXPECT_EQ(row.typlen, 0);
    EXPECT_FALSE(row.typbyval);
    EXPECT_EQ(row.typtype, pgcpp::catalog::TypeType::kBase);
    EXPECT_TRUE(row.typisdefined);
}

TEST_F(CatalogTest, PgTypeFieldAssignment) {
    FormData_pg_type row;
    row.oid = 23;
    row.typname = "int4";
    row.typlen = 4;
    row.typbyval = true;
    EXPECT_EQ(row.oid, 23u);
    EXPECT_EQ(row.typname, "int4");
    EXPECT_EQ(row.typlen, 4);
    EXPECT_TRUE(row.typbyval);
}

// ===========================================================================
// Catalog: pg_class insert / lookup
// ===========================================================================

TEST_F(CatalogTest, InsertClassAssignsOid) {
    auto* row = MakeClassRow("hits");
    Oid oid = catalog_->InsertClass(row);
    EXPECT_GE(oid, kFirstNormalObjectId);
    EXPECT_EQ(row->oid, oid);
    EXPECT_EQ(catalog_->ClassCount(), 1u);
}

TEST_F(CatalogTest, InsertClassWithExplicitOid) {
    auto* row = MakeClassRow("hits", 20000);
    Oid oid = catalog_->InsertClass(row);
    EXPECT_EQ(oid, 20000u);
}

TEST_F(CatalogTest, InsertClassDuplicateOidErrors) {
    auto* row1 = MakeClassRow("hits", 20000);
    catalog_->InsertClass(row1);
    auto* row2 = MakeClassRow("hits2", 20000);
    EXPECT_TRUE(RaisesError([&] { catalog_->InsertClass(row2); }));
}

TEST_F(CatalogTest, GetClassByOid) {
    auto* row = MakeClassRow("hits", 20000);
    catalog_->InsertClass(row);
    const FormData_pg_class* found = catalog_->GetClassByOid(20000);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->relname, "hits");
}

TEST_F(CatalogTest, GetClassByOidNotFound) {
    EXPECT_EQ(catalog_->GetClassByOid(99999), nullptr);
}

TEST_F(CatalogTest, GetClassByName) {
    auto* row = MakeClassRow("hits", 20000);
    catalog_->InsertClass(row);
    const FormData_pg_class* found = catalog_->GetClassByName("hits");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->oid, 20000u);
}

TEST_F(CatalogTest, GetClassByNameNotFound) {
    EXPECT_EQ(catalog_->GetClassByName("nonexistent"), nullptr);
}

// ===========================================================================
// Catalog: pg_class update / delete
// ===========================================================================

TEST_F(CatalogTest, UpdateClass) {
    auto* row = MakeClassRow("hits", 20000);
    row->relnatts = 3;
    catalog_->InsertClass(row);

    FormData_pg_class new_row;
    new_row.relname = "hits";
    new_row.relnatts = 5;
    EXPECT_TRUE(catalog_->UpdateClass(20000, &new_row));

    const FormData_pg_class* found = catalog_->GetClassByOid(20000);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->relnatts, 5);
}

TEST_F(CatalogTest, UpdateClassNotFound) {
    FormData_pg_class new_row;
    EXPECT_FALSE(catalog_->UpdateClass(99999, &new_row));
}

TEST_F(CatalogTest, DeleteClass) {
    auto* row = MakeClassRow("hits", 20000);
    catalog_->InsertClass(row);
    EXPECT_EQ(catalog_->ClassCount(), 1u);
    EXPECT_TRUE(catalog_->DeleteClass(20000));
    EXPECT_EQ(catalog_->ClassCount(), 0u);
    EXPECT_EQ(catalog_->GetClassByOid(20000), nullptr);
}

TEST_F(CatalogTest, DeleteClassNotFound) {
    EXPECT_FALSE(catalog_->DeleteClass(99999));
}

// ===========================================================================
// Catalog: pg_attribute insert / lookup
// ===========================================================================

TEST_F(CatalogTest, InsertAttribute) {
    auto* row = MakeAttributeRow(20000, "id", 1, 23);
    catalog_->InsertAttribute(row);
    const FormData_pg_attribute* found = catalog_->GetAttribute(20000, 1);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->attname, "id");
    EXPECT_EQ(found->atttypid, 23u);
}

TEST_F(CatalogTest, GetAttributeNotFound) {
    EXPECT_EQ(catalog_->GetAttribute(99999, 1), nullptr);
    EXPECT_EQ(catalog_->GetAttribute(20000, 99), nullptr);
}

TEST_F(CatalogTest, GetAttributesReturnsSortedByAttnum) {
    // Insert out of order.
    catalog_->InsertAttribute(MakeAttributeRow(20000, "c", 3, 25));
    catalog_->InsertAttribute(MakeAttributeRow(20000, "a", 1, 23));
    catalog_->InsertAttribute(MakeAttributeRow(20000, "b", 2, 25));

    auto attrs = catalog_->GetAttributes(20000);
    ASSERT_EQ(attrs.size(), 3u);
    EXPECT_EQ(attrs[0]->attnum, 1);
    EXPECT_EQ(attrs[1]->attnum, 2);
    EXPECT_EQ(attrs[2]->attnum, 3);
    EXPECT_EQ(attrs[0]->attname, "a");
    EXPECT_EQ(attrs[1]->attname, "b");
    EXPECT_EQ(attrs[2]->attname, "c");
}

TEST_F(CatalogTest, DeleteAttributesByRelid) {
    catalog_->InsertAttribute(MakeAttributeRow(20000, "a", 1, 23));
    catalog_->InsertAttribute(MakeAttributeRow(20000, "b", 2, 25));
    catalog_->InsertAttribute(MakeAttributeRow(20001, "x", 1, 23));

    EXPECT_EQ(catalog_->DeleteAttributes(20000), 2u);
    EXPECT_EQ(catalog_->GetAttributes(20000).size(), 0u);
    EXPECT_EQ(catalog_->GetAttributes(20001).size(), 1u);
}

// ===========================================================================
// Catalog: pg_type insert / lookup
// ===========================================================================

TEST_F(CatalogTest, InsertType) {
    auto* row = MakeTypeRow("int4", 23);
    catalog_->InsertType(row);
    const FormData_pg_type* found = catalog_->GetTypeByOid(23);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->typname, "int4");
}

TEST_F(CatalogTest, GetTypeByName) {
    auto* row = MakeTypeRow("int4", 23);
    catalog_->InsertType(row);
    const FormData_pg_type* found = catalog_->GetTypeByName("int4");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->oid, 23u);
}

TEST_F(CatalogTest, GetTypeNotFound) {
    EXPECT_EQ(catalog_->GetTypeByOid(99999), nullptr);
    EXPECT_EQ(catalog_->GetTypeByName("nonexistent"), nullptr);
}

// ===========================================================================
// Catalog: OID allocation
// ===========================================================================

TEST_F(CatalogTest, AllocateOidIncrements) {
    Oid oid1 = catalog_->AllocateOid();
    Oid oid2 = catalog_->AllocateOid();
    EXPECT_EQ(oid2, oid1 + 1);
}

TEST_F(CatalogTest, AllocateOidStartsAtFirstNormal) {
    Oid oid = catalog_->AllocateOid();
    EXPECT_EQ(oid, kFirstNormalObjectId);
}

// ===========================================================================
// CatalogTuple* API (global catalog)
// ===========================================================================

TEST_F(CatalogTest, CatalogTupleInsertReturnsOid) {
    auto* row = MakeClassRow("hits");
    Oid oid = CatalogTupleInsert(row);
    EXPECT_GE(oid, kFirstNormalObjectId);
    EXPECT_EQ(GetCatalog()->GetClassByOid(oid)->relname, "hits");
}

TEST_F(CatalogTest, CatalogTupleUpdate) {
    auto* row = MakeClassRow("hits", 20000);
    row->relnatts = 3;
    CatalogTupleInsert(row);

    FormData_pg_class new_row;
    new_row.relname = "hits";
    new_row.relnatts = 7;
    EXPECT_TRUE(CatalogTupleUpdate(20000, &new_row));
    EXPECT_EQ(GetCatalog()->GetClassByOid(20000)->relnatts, 7);
}

TEST_F(CatalogTest, CatalogTupleDelete) {
    auto* row = MakeClassRow("hits", 20000);
    CatalogTupleInsert(row);
    EXPECT_TRUE(CatalogTupleDelete(20000));
    EXPECT_EQ(GetCatalog()->GetClassByOid(20000), nullptr);
}

// ===========================================================================
// SysCache: pg_class lookups
// ===========================================================================

TEST_F(CatalogTest, SysCacheSearchClassByOidHit) {
    auto* row = MakeClassRow("hits", 20000);
    catalog_->InsertClass(row);

    const FormData_pg_class* found = syscache_->SearchClassByOid(20000);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->relname, "hits");
    EXPECT_EQ(syscache_->ClassCacheSize(), 1u);
}

TEST_F(CatalogTest, SysCacheSearchClassByOidMiss) {
    EXPECT_EQ(syscache_->SearchClassByOid(99999), nullptr);
}

TEST_F(CatalogTest, SysCacheSearchClassByNameHit) {
    auto* row = MakeClassRow("hits", 20000);
    catalog_->InsertClass(row);

    const FormData_pg_class* found = syscache_->SearchClassByName("hits", kInvalidOid);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->oid, 20000u);
}

TEST_F(CatalogTest, SysCacheSearchClassByNameMiss) {
    EXPECT_EQ(syscache_->SearchClassByName("nonexistent", kInvalidOid), nullptr);
}

TEST_F(CatalogTest, SysCacheClassLookupCachesResult) {
    auto* row = MakeClassRow("hits", 20000);
    catalog_->InsertClass(row);

    // First lookup: miss in cache, falls back to Catalog, caches result.
    const FormData_pg_class* found1 = syscache_->SearchClassByOid(20000);
    ASSERT_NE(found1, nullptr);
    EXPECT_EQ(syscache_->ClassCacheSize(), 1u);

    // Second lookup: should hit the cache (same pointer).
    const FormData_pg_class* found2 = syscache_->SearchClassByOid(20000);
    EXPECT_EQ(found1, found2);
}

// ===========================================================================
// SysCache: pg_attribute lookups
// ===========================================================================

TEST_F(CatalogTest, SysCacheSearchAttributeByNum) {
    catalog_->InsertAttribute(MakeAttributeRow(20000, "id", 1, 23));

    const FormData_pg_attribute* found = syscache_->SearchAttributeByNum(20000, 1);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->attname, "id");
}

TEST_F(CatalogTest, SysCacheSearchAttributeByName) {
    catalog_->InsertAttribute(MakeAttributeRow(20000, "id", 1, 23));

    const FormData_pg_attribute* found = syscache_->SearchAttributeByName(20000, "id");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->attnum, 1);
}

TEST_F(CatalogTest, SysCacheAttributeLookupMiss) {
    EXPECT_EQ(syscache_->SearchAttributeByNum(99999, 1), nullptr);
    EXPECT_EQ(syscache_->SearchAttributeByName(99999, "nonexistent"), nullptr);
}

// ===========================================================================
// SysCache: pg_type lookups
// ===========================================================================

TEST_F(CatalogTest, SysCacheSearchTypeByOid) {
    catalog_->InsertType(MakeTypeRow("int4", 23));

    const FormData_pg_type* found = syscache_->SearchTypeByOid(23);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->typname, "int4");
}

TEST_F(CatalogTest, SysCacheSearchTypeByName) {
    catalog_->InsertType(MakeTypeRow("int4", 23));

    const FormData_pg_type* found = syscache_->SearchTypeByName("int4", kInvalidOid);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->oid, 23u);
}

TEST_F(CatalogTest, SysCacheTypeLookupMiss) {
    EXPECT_EQ(syscache_->SearchTypeByOid(99999), nullptr);
    EXPECT_EQ(syscache_->SearchTypeByName("nonexistent", kInvalidOid), nullptr);
}

// ===========================================================================
// SysCache: invalidate
// ===========================================================================

TEST_F(CatalogTest, SysCacheInvalidateClearsEntries) {
    catalog_->InsertClass(MakeClassRow("hits", 20000));
    catalog_->InsertType(MakeTypeRow("int4", 23));

    // Populate cache.
    syscache_->SearchClassByOid(20000);
    syscache_->SearchTypeByOid(23);
    EXPECT_EQ(syscache_->ClassCacheSize(), 1u);
    EXPECT_EQ(syscache_->TypeCacheSize(), 1u);

    syscache_->Invalidate();
    EXPECT_EQ(syscache_->ClassCacheSize(), 0u);
    EXPECT_EQ(syscache_->TypeCacheSize(), 0u);
}

// ===========================================================================
// SysCache: pin / release
// ===========================================================================

TEST_F(CatalogTest, SysCacheReleaseDoesNotCrash) {
    catalog_->InsertClass(MakeClassRow("hits", 20000));
    const FormData_pg_class* found = syscache_->SearchClassByOid(20000);
    ASSERT_NE(found, nullptr);
    // Releasing should not crash and should decrement refcount.
    syscache_->Release(found);
    // Re-releasing (over-release) should be safe (lenient).
    syscache_->Release(found);
}

TEST_F(CatalogTest, SysCacheReleaseNullIsSafe) {
    syscache_->Release(nullptr);
}

// ===========================================================================
// SearchSysCache1 / SearchSysCache2 / ReleaseSysCache (compat API)
// ===========================================================================

TEST_F(CatalogTest, SearchSysCache1ClassOid) {
    catalog_->InsertClass(MakeClassRow("hits", 20000));
    const void* result = SearchSysCache1(SysCacheIdentifier::kClassOid, 20000);
    ASSERT_NE(result, nullptr);
    auto* row = static_cast<const FormData_pg_class*>(result);
    EXPECT_EQ(row->relname, "hits");
    ReleaseSysCache(result);
}

TEST_F(CatalogTest, SearchSysCache1TypeOid) {
    catalog_->InsertType(MakeTypeRow("int4", 23));
    const void* result = SearchSysCache1(SysCacheIdentifier::kTypeOid, 23);
    ASSERT_NE(result, nullptr);
    auto* row = static_cast<const FormData_pg_type*>(result);
    EXPECT_EQ(row->typname, "int4");
    ReleaseSysCache(result);
}

TEST_F(CatalogTest, SearchSysCache1AttributeRelidNum) {
    catalog_->InsertAttribute(MakeAttributeRow(20000, "id", 1, 23));
    // Pack (relid, attnum) into uintptr_t: relid in high 16 bits, attnum in low 16.
    uintptr_t key = (static_cast<uintptr_t>(20000) << 16) | static_cast<uint16_t>(1);
    const void* result = SearchSysCache1(SysCacheIdentifier::kAttributeRelidNum, key);
    ASSERT_NE(result, nullptr);
    auto* row = static_cast<const FormData_pg_attribute*>(result);
    EXPECT_EQ(row->attname, "id");
    ReleaseSysCache(result);
}

TEST_F(CatalogTest, SearchSysCache2ClassName) {
    catalog_->InsertClass(MakeClassRow("hits", 20000));
    std::string name = "hits";
    const void* result =
        SearchSysCache2(SysCacheIdentifier::kClassName, reinterpret_cast<uintptr_t>(&name),
                        static_cast<uintptr_t>(kInvalidOid));
    ASSERT_NE(result, nullptr);
    auto* row = static_cast<const FormData_pg_class*>(result);
    EXPECT_EQ(row->oid, 20000u);
    ReleaseSysCache(result);
}

TEST_F(CatalogTest, SearchSysCache2TypeName) {
    catalog_->InsertType(MakeTypeRow("int4", 23));
    std::string name = "int4";
    const void* result =
        SearchSysCache2(SysCacheIdentifier::kTypeName, reinterpret_cast<uintptr_t>(&name),
                        static_cast<uintptr_t>(kInvalidOid));
    ASSERT_NE(result, nullptr);
    auto* row = static_cast<const FormData_pg_type*>(result);
    EXPECT_EQ(row->oid, 23u);
    ReleaseSysCache(result);
}

TEST_F(CatalogTest, SearchSysCache2AttributeRelidName) {
    catalog_->InsertAttribute(MakeAttributeRow(20000, "id", 1, 23));
    std::string name = "id";
    const void* result =
        SearchSysCache2(SysCacheIdentifier::kAttributeRelidName, static_cast<uintptr_t>(20000),
                        reinterpret_cast<uintptr_t>(&name));
    ASSERT_NE(result, nullptr);
    auto* row = static_cast<const FormData_pg_attribute*>(result);
    EXPECT_EQ(row->attnum, 1);
    ReleaseSysCache(result);
}

TEST_F(CatalogTest, SearchSysCache1MissReturnsNull) {
    EXPECT_EQ(SearchSysCache1(SysCacheIdentifier::kClassOid, 99999), nullptr);
    EXPECT_EQ(SearchSysCache1(SysCacheIdentifier::kTypeOid, 99999), nullptr);
}

// ===========================================================================
// End-to-end: create a table-like catalog entry with attributes
// ===========================================================================

TEST_F(CatalogTest, EndToEndCreateTableWithAttributes) {
    // Create a pg_class entry for table "hits" with 3 columns.
    auto* class_row = MakeClassRow("hits", 20000);
    class_row->relnatts = 3;
    class_row->relkind = RelKind::kRelation;
    catalog_->InsertClass(class_row);

    // Create pg_attribute entries.
    catalog_->InsertAttribute(MakeAttributeRow(20000, "id", 1, 23));    // int4
    catalog_->InsertAttribute(MakeAttributeRow(20000, "url", 2, 25));   // text
    catalog_->InsertAttribute(MakeAttributeRow(20000, "hits", 3, 20));  // int8

    // Look up via SysCache.
    const FormData_pg_class* rel = syscache_->SearchClassByName("hits", kInvalidOid);
    ASSERT_NE(rel, nullptr);
    EXPECT_EQ(rel->relnatts, 3);

    auto attrs = catalog_->GetAttributes(rel->oid);
    ASSERT_EQ(attrs.size(), 3u);
    EXPECT_EQ(attrs[0]->attname, "id");
    EXPECT_EQ(attrs[1]->attname, "url");
    EXPECT_EQ(attrs[2]->attname, "hits");

    // Look up a specific attribute via SysCache.
    const FormData_pg_attribute* attr = syscache_->SearchAttributeByName(rel->oid, "url");
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(attr->atttypid, 25u);  // text OID
}

// ===========================================================================
// A-3: persistence (Save / Load round-trip)
// ===========================================================================

// A-3: Saving a catalog with user rows, then loading into a fresh catalog
// restores the rows with full field fidelity and restores next_oid_.
TEST_F(CatalogTest, SaveLoadRoundTripPreservesUserRows) {
    // Build a user table: one pg_class row (auto-OID), two pg_attribute rows,
    // and a pg_type row (auto-OID). Set several non-default fields to verify
    // they survive the round-trip.
    auto* class_row = MakeClassRow("mytable");  // oid auto-assigned -> 16384
    class_row->relnatts = 2;
    class_row->relkind = RelKind::kRelation;
    class_row->relpersistence = RelPersistence::kPermanent;
    class_row->relhasindex = true;
    class_row->relispopulated = true;
    class_row->relfilenode = 16390;
    Oid class_oid = catalog_->InsertClass(class_row);
    EXPECT_EQ(class_oid, kFirstNormalObjectId);

    auto* attr1 = MakeAttributeRow(class_oid, "id", 1, 23);
    attr1->attlen = 4;
    attr1->attbyval = true;
    attr1->attnotnull = true;
    catalog_->InsertAttribute(attr1);
    auto* attr2 = MakeAttributeRow(class_oid, "name", 2, 25);
    attr2->attisdropped = true;  // exercise a bool field
    catalog_->InsertAttribute(attr2);

    auto* type_row = MakeTypeRow("mytype");  // oid auto-assigned -> 16385
    type_row->typlen = 4;
    type_row->typbyval = true;
    type_row->typdefault = "0";
    Oid type_oid = catalog_->InsertType(type_row);
    EXPECT_EQ(type_oid, kFirstNormalObjectId + 1);

    // next_oid_ is now 16386 after two allocations.
    std::string path = "/tmp/pgcpp_catalog_test_" + std::to_string(getpid()) + ".tsv";
    ASSERT_TRUE(catalog_->Save(path));

    // Load into a fresh catalog (still in the same memory context so palloc'd
    // rows are tracked).
    Catalog catalog2;
    ASSERT_TRUE(catalog2.Load(path));

    // pg_class restored with field fidelity.
    const FormData_pg_class* restored_class = catalog2.GetClassByName("mytable");
    ASSERT_NE(restored_class, nullptr);
    EXPECT_EQ(restored_class->oid, class_oid);
    EXPECT_EQ(restored_class->relnatts, 2);
    EXPECT_EQ(restored_class->relkind, RelKind::kRelation);
    EXPECT_EQ(restored_class->relpersistence, RelPersistence::kPermanent);
    EXPECT_TRUE(restored_class->relhasindex);
    EXPECT_TRUE(restored_class->relispopulated);
    EXPECT_EQ(restored_class->relfilenode, 16390u);

    // pg_attribute restored (both rows).
    auto attrs = catalog2.GetAttributes(class_oid);
    ASSERT_EQ(attrs.size(), 2u);
    EXPECT_EQ(attrs[0]->attname, "id");
    EXPECT_EQ(attrs[0]->attnum, 1);
    EXPECT_EQ(attrs[0]->atttypid, 23u);
    EXPECT_EQ(attrs[0]->attlen, 4);
    EXPECT_TRUE(attrs[0]->attbyval);
    EXPECT_TRUE(attrs[0]->attnotnull);
    EXPECT_EQ(attrs[1]->attname, "name");
    EXPECT_TRUE(attrs[1]->attisdropped);

    // pg_type restored.
    const FormData_pg_type* restored_type = catalog2.GetTypeByOid(type_oid);
    ASSERT_NE(restored_type, nullptr);
    EXPECT_EQ(restored_type->typname, "mytype");
    EXPECT_EQ(restored_type->typlen, 4);
    EXPECT_TRUE(restored_type->typbyval);
    EXPECT_EQ(restored_type->typdefault, "0");

    // next_oid_ restored: the next allocation continues past loaded rows.
    EXPECT_EQ(catalog2.AllocateOid(), kFirstNormalObjectId + 2);

    std::remove(path.c_str());
}

// A-3: built-in rows (oid < kFirstNormalObjectId) are NOT persisted — only
// user-created rows survive a round-trip.
TEST_F(CatalogTest, SaveOmitsBuiltinRows) {
    auto* builtin_row = MakeClassRow("builtin_rel", 100);
    catalog_->InsertClass(builtin_row);
    auto* user_row = MakeClassRow("user_rel", 20000);
    catalog_->InsertClass(user_row);

    std::string path = "/tmp/pgcpp_catalog_test_" + std::to_string(getpid()) + ".tsv";
    ASSERT_TRUE(catalog_->Save(path));

    Catalog catalog2;
    ASSERT_TRUE(catalog2.Load(path));

    EXPECT_EQ(catalog2.GetClassByOid(100), nullptr);  // builtin not restored
    ASSERT_NE(catalog2.GetClassByOid(20000), nullptr);
    EXPECT_EQ(catalog2.GetClassByName("user_rel")->oid, 20000u);

    std::remove(path.c_str());
}

// A-3: Loading a non-existent file is not an error (fresh initdb path).
TEST_F(CatalogTest, LoadMissingFileReturnsFalse) {
    Catalog catalog2;
    EXPECT_FALSE(catalog2.Load("/tmp/pgcpp_does_not_exist_12345.tsv"));
}

// A-3: an empty catalog (no user rows) still saves/loads cleanly.
TEST_F(CatalogTest, SaveLoadEmptyCatalog) {
    std::string path = "/tmp/pgcpp_catalog_test_" + std::to_string(getpid()) + ".tsv";
    ASSERT_TRUE(catalog_->Save(path));

    Catalog catalog2;
    ASSERT_TRUE(catalog2.Load(path));
    EXPECT_EQ(catalog2.ClassCount(), 0u);
    EXPECT_EQ(catalog2.AllocateOid(), kFirstNormalObjectId);

    std::remove(path.c_str());
}

}  // namespace
