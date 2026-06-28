// namespace_test.cpp — unit tests for schema/namespace resolution
// (M3 Task 15.6.4).
//
// Verifies RangeVarGetRelid, RelnameGetRelid, makeRangeVarFromNameList, and
// get_namespace_name against a manually-inserted pg_class row. pgcpp has a
// single implicit "public" namespace; schemaname is ignored.
#include "pgcpp/catalog/namespace.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_class.hpp"
#include "pgcpp/catalog/syscache.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/parser/parsenodes.hpp"

namespace {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::get_namespace_name;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::makeRangeVarFromNameList;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RangeVarGetRelid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelnameGetRelid;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::RangeVar;

constexpr Oid kHitsOid = 20000;

class NamespaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("namespace_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        // Insert a pg_class row for the "hits" relation.
        auto* cls = makePallocNode<FormData_pg_class>();
        cls->oid = kHitsOid;
        cls->relname = "hits";
        cls->relkind = RelKind::kRelation;
        cls->relpersistence = RelPersistence::kPermanent;
        catalog_->InsertClass(cls);
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

    // Helper: build a RangeVar with the given relname.
    RangeVar* MakeRangeVar(const std::string& relname) {
        auto* rv = makePallocNode<RangeVar>();
        rv->relname = relname;
        return rv;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

TEST_F(NamespaceTest, RangeVarGetRelid_ResolvesExistingName) {
    auto* rv = MakeRangeVar("hits");
    EXPECT_EQ(RangeVarGetRelid(rv, /*failOK=*/true), kHitsOid);
}

TEST_F(NamespaceTest, RangeVarGetRelid_ReturnsInvalidForMissing_WhenFailOK) {
    auto* rv = MakeRangeVar("nonexistent");
    EXPECT_EQ(RangeVarGetRelid(rv, /*failOK=*/true), kInvalidOid);
}

TEST_F(NamespaceTest, RangeVarGetRelid_HandlesNullRangeVar_WhenFailOK) {
    // A null RangeVar with failOK=true must not ereport; it returns InvalidOid.
    EXPECT_EQ(RangeVarGetRelid(nullptr, /*failOK=*/true), kInvalidOid);
}

TEST_F(NamespaceTest, RelnameGetRelid_ResolvesExistingName) {
    EXPECT_EQ(RelnameGetRelid("hits"), kHitsOid);
}

TEST_F(NamespaceTest, RelnameGetRelid_ReturnsInvalidForMissing) {
    // Default failOK=true returns InvalidOid on miss (no ereport).
    EXPECT_EQ(RelnameGetRelid("nonexistent"), kInvalidOid);
}

TEST_F(NamespaceTest, get_namespace_name_ReturnsPublic) {
    // pgcpp has a single implicit namespace; any OID maps to "public".
    EXPECT_STREQ(get_namespace_name(2200), "public");
    EXPECT_STREQ(get_namespace_name(kInvalidOid), "public");
}

TEST_F(NamespaceTest, makeRangeVarFromNameList_SingleName) {
    auto* rv = makeRangeVarFromNameList({"hits"});
    ASSERT_NE(rv, nullptr);
    EXPECT_EQ(rv->relname, "hits");
    EXPECT_TRUE(rv->schemaname.empty());
    EXPECT_TRUE(rv->catalogname.empty());
}

TEST_F(NamespaceTest, makeRangeVarFromNameList_TwoNamesSetsSchema) {
    auto* rv = makeRangeVarFromNameList({"public", "hits"});
    ASSERT_NE(rv, nullptr);
    EXPECT_EQ(rv->relname, "hits");
    EXPECT_EQ(rv->schemaname, "public");
    EXPECT_TRUE(rv->catalogname.empty());
}

TEST_F(NamespaceTest, makeRangeVarFromNameList_ThreeNamesSetsCatalogAndSchema) {
    auto* rv = makeRangeVarFromNameList({"mydb", "public", "hits"});
    ASSERT_NE(rv, nullptr);
    EXPECT_EQ(rv->relname, "hits");
    EXPECT_EQ(rv->schemaname, "public");
    EXPECT_EQ(rv->catalogname, "mydb");
}

TEST_F(NamespaceTest, makeRangeVarFromNameList_EmptyListReturnsNull) {
    EXPECT_EQ(makeRangeVarFromNameList({}), nullptr);
}

}  // namespace
