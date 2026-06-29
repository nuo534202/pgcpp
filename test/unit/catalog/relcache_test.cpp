// relcache_test.cpp — unit tests for the relation cache (M3 Task 15.6.1).
//
// Verifies RelationOpen/Close, the RelationIdGetRelation alias, the
// RelationBuildDesc cache-miss path, RelationCacheInvalidate, and
// RelationGetNumberOfAttributes.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace {

using pgcpp::access::InitializeRelcache;
using pgcpp::access::Relation;
using pgcpp::access::RelationBuildDesc;
using pgcpp::access::RelationCacheInvalidate;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCloseByOid;
using pgcpp::access::RelationGetNumberOfAttributes;
using pgcpp::access::RelationIdGetRelation;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

class RelcacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("relcache_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        InitializeRelcache();

        // Insert a relation "hits" with OID 20000 and 3 attributes.
        auto* class_row = makePallocNode<FormData_pg_class>();
        class_row->oid = 20000;
        class_row->relname = "hits";
        class_row->relnatts = 3;
        class_row->relkind = RelKind::kRelation;
        class_row->relpersistence = RelPersistence::kPermanent;
        // relfilenode left as InvalidOid so RelationOpen won't touch smgr.
        catalog_->InsertClass(class_row);

        InsertAttribute(20000, "id", 1, 23);    // int4
        InsertAttribute(20000, "url", 2, 25);   // text
        InsertAttribute(20000, "hits", 3, 20);  // int8
    }

    void TearDown() override {
        ResetRelcache();
        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    void InsertAttribute(Oid relid, const std::string& name, int16_t attnum, Oid typid) {
        auto* row = makePallocNode<FormData_pg_attribute>();
        row->attrelid = relid;
        row->attname = name;
        row->attnum = attnum;
        row->atttypid = typid;
        catalog_->InsertAttribute(row);
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

TEST_F(RelcacheTest, RelationOpen_ReturnsRelationForExistingOid) {
    Relation rel = RelationOpen(20000);
    ASSERT_NE(rel, nullptr);
    EXPECT_EQ(rel->rd_id, 20000u);
    EXPECT_EQ(rel->rd_refcnt, 1);
    ASSERT_NE(rel->rd_rel, nullptr);
    EXPECT_EQ(rel->rd_rel->relname, "hits");
    ASSERT_NE(rel->rd_att, nullptr);
    EXPECT_EQ(rel->rd_att->natts, 3);
    EXPECT_TRUE(rel->rd_isvalid);
    RelationClose(rel);
}

TEST_F(RelcacheTest, RelationOpen_ReturnsNullForMissingOid) {
    Relation rel = RelationOpen(99999);
    EXPECT_EQ(rel, nullptr);
}

TEST_F(RelcacheTest, RelationClose_DecrementsRefcnt) {
    Relation rel = RelationOpen(20000);
    ASSERT_NE(rel, nullptr);
    EXPECT_EQ(rel->rd_refcnt, 1);
    RelationClose(rel);
    EXPECT_EQ(rel->rd_refcnt, 0);
}

TEST_F(RelcacheTest, RelationIdGetRelation_AliasWorks) {
    Relation rel_open = RelationOpen(20000);
    ASSERT_NE(rel_open, nullptr);
    // Second open via the alias should return the same cached pointer with
    // refcnt bumped.
    Relation rel_alias = RelationIdGetRelation(20000);
    ASSERT_NE(rel_alias, nullptr);
    EXPECT_EQ(rel_open, rel_alias);
    EXPECT_EQ(rel_alias->rd_refcnt, 2);
    RelationClose(rel_alias);
    RelationClose(rel_open);
}

TEST_F(RelcacheTest, RelationOpen_CachesInRelcache) {
    Relation rel1 = RelationOpen(20000);
    ASSERT_NE(rel1, nullptr);
    EXPECT_EQ(rel1->rd_refcnt, 1);
    Relation rel2 = RelationOpen(20000);
    ASSERT_NE(rel2, nullptr);
    EXPECT_EQ(rel1, rel2);
    EXPECT_EQ(rel2->rd_refcnt, 2);
    RelationClose(rel1);
    RelationClose(rel2);
    EXPECT_EQ(rel2->rd_refcnt, 0);
}

TEST_F(RelcacheTest, RelationCacheInvalidate_RemovesEntry) {
    Relation rel = RelationOpen(20000);
    ASSERT_NE(rel, nullptr);
    EXPECT_EQ(rel->rd_refcnt, 1);

    RelationCacheInvalidate(20000);
    EXPECT_EQ(rel->rd_refcnt, 0);

    // After invalidation, a fresh open should produce a NEW RelationData.
    Relation rel_after = RelationOpen(20000);
    ASSERT_NE(rel_after, nullptr);
    EXPECT_NE(rel_after, rel);
    EXPECT_EQ(rel_after->rd_refcnt, 1);
    RelationClose(rel_after);
    // rel was orphaned by RelationCacheInvalidate (erased from cache but not
    // destroyed). Explicitly destroy it to avoid a double-destructor at
    // MemoryContext deletion (~RelationData would re-destroy rd_att).
    destroyPallocNode(rel);
}

TEST_F(RelcacheTest, RelationCacheInvalidate_NoOpForMissingOid) {
    // Should not crash on an OID that was never cached.
    RelationCacheInvalidate(99999);
    // Sanity: the existing entry should still be present.
    Relation rel = RelationOpen(20000);
    ASSERT_NE(rel, nullptr);
    RelationClose(rel);
}

TEST_F(RelcacheTest, RelationCloseByOid_DecrementsRefcnt) {
    Relation rel = RelationOpen(20000);
    ASSERT_NE(rel, nullptr);
    EXPECT_EQ(rel->rd_refcnt, 1);
    RelationCloseByOid(20000);
    EXPECT_EQ(rel->rd_refcnt, 0);
    // No-op for an OID not in the cache (already removed via decrement path
    // would not happen here — entry stays in cache at refcnt 0).
    RelationCloseByOid(99999);  // should not crash
}

TEST_F(RelcacheTest, RelationGetNumberOfAttributes) {
    Relation rel = RelationOpen(20000);
    ASSERT_NE(rel, nullptr);
    EXPECT_EQ(RelationGetNumberOfAttributes(rel), 3);
    RelationClose(rel);

    // Null relation is safe.
    EXPECT_EQ(RelationGetNumberOfAttributes(nullptr), 0);
}

TEST_F(RelcacheTest, RelationBuildDesc_BuildsFromCatalog) {
    // RelationBuildDesc bypasses the cache: calling it twice must produce
    // distinct RelationData pointers (each with rd_refcnt == 1).
    Relation a = RelationBuildDesc(20000);
    Relation b = RelationBuildDesc(20000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_EQ(a->rd_refcnt, 1);
    EXPECT_EQ(b->rd_refcnt, 1);
    EXPECT_EQ(a->rd_id, 20000u);
    EXPECT_EQ(b->rd_id, 20000u);
    EXPECT_EQ(RelationGetNumberOfAttributes(a), 3);
    EXPECT_EQ(RelationGetNumberOfAttributes(b), 3);
    // a and b were never inserted into the cache; destroy them explicitly so
    // their rd_att descriptors do not double-destruct at context deletion.
    destroyPallocNode(a);
    destroyPallocNode(b);
}

TEST_F(RelcacheTest, RelationBuildDesc_ReturnsNullForMissingOid) {
    EXPECT_EQ(RelationBuildDesc(99999), nullptr);
}

TEST_F(RelcacheTest, RelationClearRelation_RemovesFromCache) {
    Relation rel = RelationOpen(20000);
    ASSERT_NE(rel, nullptr);
    RelationClearRelation(rel);
    EXPECT_EQ(rel->rd_refcnt, 0);
    // A fresh open builds a new descriptor.
    Relation rel_after = RelationOpen(20000);
    ASSERT_NE(rel_after, nullptr);
    EXPECT_NE(rel_after, rel);
    RelationClose(rel_after);
    // rel was orphaned by RelationClearRelation; destroy it explicitly.
    destroyPallocNode(rel);
}

}  // namespace
