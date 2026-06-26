#include "mytoydb/catalog/catalog.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "mytoydb/catalog/pg_attribute.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/catalog/pg_type.hpp"
#include "mytoydb/catalog/syscache.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

namespace {

using mytoydb::catalog::Catalog;
using mytoydb::catalog::CatalogTupleDelete;
using mytoydb::catalog::CatalogTupleInsert;
using mytoydb::catalog::CatalogTupleUpdate;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::FormData_pg_type;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::GetSysCache;
using mytoydb::catalog::kFirstNormalObjectId;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::ReleaseSysCache;
using mytoydb::catalog::RelKind;
using mytoydb::catalog::RelPersistence;
using mytoydb::catalog::SearchSysCache1;
using mytoydb::catalog::SearchSysCache2;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::catalog::SysCacheIdentifier;
using mytoydb::error::ErrorData;
using mytoydb::error::LogLevel;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::makePallocNode;

class CatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("catalog_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

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

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
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
        ErrorData* err = mytoydb::error::GetErrorData();
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
    EXPECT_EQ(row.typtype, mytoydb::catalog::TypeType::kBase);
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

}  // namespace
