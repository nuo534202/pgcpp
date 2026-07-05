// catalog_p06_test.cpp — unit tests for P0-6 system catalog tables.
//
// Covers CRUD accessors and TSV persistence (Save/Load round-trip) for the
// 12 new catalog tables introduced in P0-6:
//   pg_namespace, pg_database, pg_index, pg_constraint, pg_attrdef,
//   pg_depend, pg_statistic, pg_inherits, pg_am, pg_tablespace,
//   pg_trigger, pg_rewrite.

#include "catalog/catalog.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "catalog/pg_am.hpp"
#include "catalog/pg_attrdef.hpp"
#include "catalog/pg_constraint.hpp"
#include "catalog/pg_database.hpp"
#include "catalog/pg_depend.hpp"
#include "catalog/pg_index.hpp"
#include "catalog/pg_inherits.hpp"
#include "catalog/pg_namespace.hpp"
#include "catalog/pg_rewrite.hpp"
#include "catalog/pg_statistic.hpp"
#include "catalog/pg_tablespace.hpp"
#include "catalog/pg_trigger.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::ConstraintAction;
using pgcpp::catalog::ConstraintMatch;
using pgcpp::catalog::ConstraintType;
using pgcpp::catalog::DependencyType;
using pgcpp::catalog::FormData_pg_am;
using pgcpp::catalog::FormData_pg_attrdef;
using pgcpp::catalog::FormData_pg_constraint;
using pgcpp::catalog::FormData_pg_database;
using pgcpp::catalog::FormData_pg_depend;
using pgcpp::catalog::FormData_pg_index;
using pgcpp::catalog::FormData_pg_inherits;
using pgcpp::catalog::FormData_pg_namespace;
using pgcpp::catalog::FormData_pg_rewrite;
using pgcpp::catalog::FormData_pg_statistic;
using pgcpp::catalog::FormData_pg_tablespace;
using pgcpp::catalog::FormData_pg_trigger;
using pgcpp::catalog::AmType;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::TriggerEnabled;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;

class CatalogP06Test : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("catalog_p06_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        pgcpp::catalog::SetCatalog(catalog_);
    }

    void TearDown() override {
        pgcpp::catalog::SetCatalog(nullptr);
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    std::string TmpPath() const {
        return "/tmp/pgcpp_catalog_p06_test_" + std::to_string(getpid()) + ".tsv";
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
};

// ===========================================================================
// Struct defaults
// ===========================================================================

TEST_F(CatalogP06Test, PgNamespaceDefaults) {
    FormData_pg_namespace r;
    EXPECT_EQ(r.oid, kInvalidOid);
    EXPECT_TRUE(r.nspname.empty());
    EXPECT_EQ(r.nspowner, kInvalidOid);
    EXPECT_FALSE(r.nspacl);
}

TEST_F(CatalogP06Test, PgDatabaseDefaults) {
    FormData_pg_database r;
    EXPECT_EQ(r.oid, kInvalidOid);
    EXPECT_TRUE(r.datname.empty());
    EXPECT_EQ(r.datconnlimit, -1);
    EXPECT_TRUE(r.datallowconn);
    EXPECT_FALSE(r.datistemplate);
}

TEST_F(CatalogP06Test, PgIndexDefaults) {
    FormData_pg_index r;
    EXPECT_EQ(r.indexrelid, kInvalidOid);
    EXPECT_EQ(r.indrelid, kInvalidOid);
    EXPECT_FALSE(r.indisunique);
    EXPECT_FALSE(r.indisprimary);
    EXPECT_TRUE(r.indkey.empty());
}

TEST_F(CatalogP06Test, PgConstraintDefaults) {
    FormData_pg_constraint r;
    EXPECT_EQ(r.oid, kInvalidOid);
    EXPECT_EQ(r.contype, ConstraintType::kCheck);
    EXPECT_EQ(r.confmatchtype, ConstraintMatch::kSimple);
    EXPECT_TRUE(r.conislocal);
    EXPECT_FALSE(r.connoinherit);
}

TEST_F(CatalogP06Test, PgDependDefaults) {
    FormData_pg_depend r;
    EXPECT_EQ(r.classid, kInvalidOid);
    EXPECT_EQ(r.objid, kInvalidOid);
    EXPECT_EQ(r.deptype, DependencyType::kNormal);
}

TEST_F(CatalogP06Test, PgStatisticDefaults) {
    FormData_pg_statistic r;
    EXPECT_EQ(r.starelid, kInvalidOid);
    EXPECT_EQ(r.staattnum, 0);
    EXPECT_FLOAT_EQ(r.stanullfrac, 0.0F);
}

TEST_F(CatalogP06Test, PgAmDefaults) {
    FormData_pg_am r;
    EXPECT_EQ(r.oid, kInvalidOid);
    EXPECT_EQ(r.amtype, AmType::kIndex);
    EXPECT_FALSE(r.amcanorder);
}

TEST_F(CatalogP06Test, PgTriggerDefaults) {
    FormData_pg_trigger r;
    EXPECT_EQ(r.oid, kInvalidOid);
    EXPECT_EQ(r.tgenabled, TriggerEnabled::kOrigin);
    EXPECT_FALSE(r.tgisinternal);
}

TEST_F(CatalogP06Test, PgRewriteDefaults) {
    FormData_pg_rewrite r;
    EXPECT_EQ(r.oid, kInvalidOid);
    EXPECT_EQ(r.ev_type, 0);
    EXPECT_TRUE(r.ev_enabled);
    EXPECT_FALSE(r.is_instead);
}

// ===========================================================================
// pg_namespace CRUD
// ===========================================================================

TEST_F(CatalogP06Test, NamespaceInsertAssignsOid) {
    auto* r = makePallocNode<FormData_pg_namespace>();
    r->nspname = "myschema";
    Oid oid = catalog_->InsertNamespace(r);
    EXPECT_GE(oid, kFirstNormalObjectId);
    EXPECT_EQ(r->oid, oid);
}

TEST_F(CatalogP06Test, NamespaceGetByOidAndName) {
    auto* r = makePallocNode<FormData_pg_namespace>();
    r->nspname = "myschema";
    r->nspowner = 10;
    catalog_->InsertNamespace(r);

    ASSERT_NE(catalog_->GetNamespaceByOid(r->oid), nullptr);
    EXPECT_EQ(catalog_->GetNamespaceByOid(r->oid)->nspname, "myschema");
    ASSERT_NE(catalog_->GetNamespaceByName("myschema"), nullptr);
    EXPECT_EQ(catalog_->GetNamespaceByName("myschema")->nspowner, 10u);
}

TEST_F(CatalogP06Test, NamespaceDelete) {
    auto* r = makePallocNode<FormData_pg_namespace>();
    r->nspname = "tmp";
    Oid oid = catalog_->InsertNamespace(r);
    EXPECT_TRUE(catalog_->DeleteNamespace(oid));
    EXPECT_EQ(catalog_->GetNamespaceByOid(oid), nullptr);
    EXPECT_FALSE(catalog_->DeleteNamespace(oid));
}

TEST_F(CatalogP06Test, NamespaceGetMissingReturnsNull) {
    EXPECT_EQ(catalog_->GetNamespaceByOid(99999), nullptr);
    EXPECT_EQ(catalog_->GetNamespaceByName("nonexistent"), nullptr);
}

// ===========================================================================
// pg_database CRUD
// ===========================================================================

TEST_F(CatalogP06Test, DatabaseInsertAndGet) {
    auto* r = makePallocNode<FormData_pg_database>();
    r->datname = "mydb";
    r->encoding = 6;  // UTF8
    r->datcollate = "en_US.UTF-8";
    r->datconnlimit = 100;
    Oid oid = catalog_->InsertDatabase(r);
    EXPECT_GE(oid, kFirstNormalObjectId);

    const FormData_pg_database* got = catalog_->GetDatabaseByOid(oid);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->datname, "mydb");
    EXPECT_EQ(got->encoding, 6);
    EXPECT_EQ(got->datcollate, "en_US.UTF-8");
    EXPECT_EQ(got->datconnlimit, 100);

    const FormData_pg_database* got2 = catalog_->GetDatabaseByName("mydb");
    ASSERT_NE(got2, nullptr);
    EXPECT_EQ(got2->oid, oid);
}

TEST_F(CatalogP06Test, DatabaseDelete) {
    auto* r = makePallocNode<FormData_pg_database>();
    r->datname = "tmpdb";
    Oid oid = catalog_->InsertDatabase(r);
    EXPECT_TRUE(catalog_->DeleteDatabase(oid));
    EXPECT_EQ(catalog_->GetDatabaseByOid(oid), nullptr);
}

// ===========================================================================
// pg_index CRUD
// ===========================================================================

TEST_F(CatalogP06Test, IndexInsertAndGetByOid) {
    auto* r = makePallocNode<FormData_pg_index>();
    r->indrelid = 20000;
    r->indnatts = 2;
    r->indisunique = true;
    r->indisprimary = false;
    r->indkey = {1, 2};
    r->indcollation = {100, 100};
    r->indclass = {1979, 1979};
    Oid oid = catalog_->InsertIndex(r);

    const FormData_pg_index* got = catalog_->GetIndexByOid(oid);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->indrelid, 20000u);
    EXPECT_EQ(got->indnatts, 2);
    EXPECT_TRUE(got->indisunique);
    ASSERT_EQ(got->indkey.size(), 2u);
    EXPECT_EQ(got->indkey[0], 1);
    EXPECT_EQ(got->indkey[1], 2);
}

TEST_F(CatalogP06Test, IndexGetByRelidReturnsAll) {
    auto* r1 = makePallocNode<FormData_pg_index>();
    r1->indrelid = 20000;
    catalog_->InsertIndex(r1);
    auto* r2 = makePallocNode<FormData_pg_index>();
    r2->indrelid = 20000;
    catalog_->InsertIndex(r2);
    auto* r3 = makePallocNode<FormData_pg_index>();
    r3->indrelid = 20001;
    catalog_->InsertIndex(r3);

    auto v = catalog_->GetIndexesByRelid(20000);
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(catalog_->GetIndexesByRelid(20001).size(), 1u);
}

TEST_F(CatalogP06Test, IndexDeleteByOidAndForRelid) {
    auto* r1 = makePallocNode<FormData_pg_index>();
    r1->indrelid = 20000;
    Oid oid1 = catalog_->InsertIndex(r1);
    auto* r2 = makePallocNode<FormData_pg_index>();
    r2->indrelid = 20000;
    catalog_->InsertIndex(r2);

    EXPECT_TRUE(catalog_->DeleteIndex(oid1));
    EXPECT_EQ(catalog_->GetIndexByOid(oid1), nullptr);
    EXPECT_EQ(catalog_->GetIndexesByRelid(20000).size(), 1u);

    EXPECT_EQ(catalog_->DeleteIndexesForRelid(20000), 1u);
    EXPECT_EQ(catalog_->GetIndexesByRelid(20000).size(), 0u);
}

// ===========================================================================
// pg_constraint CRUD
// ===========================================================================

TEST_F(CatalogP06Test, ConstraintInsertAndGet) {
    auto* r = makePallocNode<FormData_pg_constraint>();
    r->conname = "mycheck";
    r->contype = ConstraintType::kCheck;
    r->conrelid = 20000;
    r->conkey = {1, 2};
    r->convalidated = true;
    r->consrc = "id > 0";
    Oid oid = catalog_->InsertConstraint(r);

    const FormData_pg_constraint* got = catalog_->GetConstraintByOid(oid);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->conname, "mycheck");
    EXPECT_EQ(got->contype, ConstraintType::kCheck);
    EXPECT_EQ(got->conrelid, 20000u);
    EXPECT_TRUE(got->convalidated);
    EXPECT_EQ(got->consrc, "id > 0");
    ASSERT_EQ(got->conkey.size(), 2u);
}

TEST_F(CatalogP06Test, ConstraintForeignKeyFields) {
    auto* r = makePallocNode<FormData_pg_constraint>();
    r->contype = ConstraintType::kForeignKey;
    r->conrelid = 20000;
    r->confkey = {1};
    r->conpfeqop = {4104};
    r->confdeltype = ConstraintAction::kCascade;
    r->confmatchtype = ConstraintMatch::kFull;
    Oid oid = catalog_->InsertConstraint(r);

    const FormData_pg_constraint* got = catalog_->GetConstraintByOid(oid);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->contype, ConstraintType::kForeignKey);
    EXPECT_EQ(got->confdeltype, ConstraintAction::kCascade);
    EXPECT_EQ(got->confmatchtype, ConstraintMatch::kFull);
    EXPECT_EQ(got->conpfeqop.size(), 1u);
}

TEST_F(CatalogP06Test, ConstraintGetByRelidAndDelete) {
    auto* r1 = makePallocNode<FormData_pg_constraint>();
    r1->conrelid = 20000;
    r1->conname = "c1";
    catalog_->InsertConstraint(r1);
    auto* r2 = makePallocNode<FormData_pg_constraint>();
    r2->conrelid = 20000;
    r2->conname = "c2";
    Oid oid2 = catalog_->InsertConstraint(r2);

    auto v = catalog_->GetConstraintsByRelid(20000);
    ASSERT_EQ(v.size(), 2u);

    EXPECT_TRUE(catalog_->DeleteConstraint(oid2));
    EXPECT_EQ(catalog_->GetConstraintsByRelid(20000).size(), 1u);
    EXPECT_EQ(catalog_->DeleteConstraintsForRelid(20000), 1u);
}

// ===========================================================================
// pg_attrdef CRUD
// ===========================================================================

TEST_F(CatalogP06Test, AttrdefInsertAndGet) {
    auto* r = makePallocNode<FormData_pg_attrdef>();
    r->adrelid = 20000;
    r->adnum = 1;
    r->adsrc = "nextval('seq')";
    Oid oid = catalog_->InsertAttrdef(r);

    const FormData_pg_attrdef* got = catalog_->GetAttrdef(20000, 1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->oid, oid);
    EXPECT_EQ(got->adsrc, "nextval('seq')");
}

TEST_F(CatalogP06Test, AttrdefGetByRelidAndDelete) {
    catalog_->InsertAttrdef([&] {
        auto* r = makePallocNode<FormData_pg_attrdef>();
        r->adrelid = 20000;
        r->adnum = 1;
        return r;
    }());
    catalog_->InsertAttrdef([&] {
        auto* r = makePallocNode<FormData_pg_attrdef>();
        r->adrelid = 20000;
        r->adnum = 2;
        return r;
    }());

    EXPECT_EQ(catalog_->GetAttrdefsByRelid(20000).size(), 2u);
    EXPECT_EQ(catalog_->DeleteAttrdefsForRelid(20000), 2u);
    EXPECT_EQ(catalog_->GetAttrdefsByRelid(20000).size(), 0u);
}

// ===========================================================================
// pg_depend CRUD
// ===========================================================================

TEST_F(CatalogP06Test, DependInsertAndLookup) {
    auto* r = makePallocNode<FormData_pg_depend>();
    r->classid = 1259;   // pg_class
    r->objid = 20000;    // user table
    r->objsubid = 0;
    r->refclassid = 1259;
    r->refobjid = 20001;  // depends on another user table
    r->deptype = DependencyType::kAuto;
    catalog_->InsertDepend(r);

    auto by_obj = catalog_->GetDependsByObj(1259, 20000);
    ASSERT_EQ(by_obj.size(), 1u);
    EXPECT_EQ(by_obj[0]->refobjid, 20001u);
    EXPECT_EQ(by_obj[0]->deptype, DependencyType::kAuto);

    auto by_ref = catalog_->GetDependsByRef(1259, 20001);
    ASSERT_EQ(by_ref.size(), 1u);
    EXPECT_EQ(by_ref[0]->objid, 20000u);
}

TEST_F(CatalogP06Test, DependDeleteByObjAndRef) {
    auto* r = makePallocNode<FormData_pg_depend>();
    r->classid = 1259;
    r->objid = 20000;
    r->refclassid = 1259;
    r->refobjid = 20001;
    catalog_->InsertDepend(r);

    EXPECT_EQ(catalog_->DeleteDependsByObj(1259, 20000), 1u);
    EXPECT_EQ(catalog_->GetDependsByRef(1259, 20001).size(), 0u);
}

// ===========================================================================
// pg_statistic CRUD
// ===========================================================================

TEST_F(CatalogP06Test, StatisticInsertAndGet) {
    auto* r = makePallocNode<FormData_pg_statistic>();
    r->starelid = 20000;
    r->staattnum = 1;
    r->stanullfrac = 0.25F;
    r->stawidth = 8;
    r->stadistinct = -1;
    r->stakind1 = 1;  // MCV
    r->stavalues1 = "1,2,3";
    catalog_->InsertStatistic(r);

    const FormData_pg_statistic* got = catalog_->GetStatistic(20000, 1);
    ASSERT_NE(got, nullptr);
    EXPECT_FLOAT_EQ(got->stanullfrac, 0.25F);
    EXPECT_EQ(got->stawidth, 8);
    EXPECT_EQ(got->stakind1, 1);
    EXPECT_EQ(got->stavalues1, "1,2,3");
}

TEST_F(CatalogP06Test, StatisticDeleteForRelid) {
    catalog_->InsertStatistic([&] {
        auto* r = makePallocNode<FormData_pg_statistic>();
        r->starelid = 20000;
        r->staattnum = 1;
        return r;
    }());
    catalog_->InsertStatistic([&] {
        auto* r = makePallocNode<FormData_pg_statistic>();
        r->starelid = 20000;
        r->staattnum = 2;
        return r;
    }());

    EXPECT_EQ(catalog_->DeleteStatisticsForRelid(20000), 2u);
    EXPECT_EQ(catalog_->GetStatistic(20000, 1), nullptr);
}

// ===========================================================================
// pg_inherits CRUD
// ===========================================================================

TEST_F(CatalogP06Test, InheritsInsertAndLookup) {
    auto* r = makePallocNode<FormData_pg_inherits>();
    r->inhrelid = 20000;  // child
    r->inhparent = 16384; // parent
    r->inhseqnum = 1;
    catalog_->InsertInherits(r);

    const FormData_pg_inherits* child = catalog_->GetInheritsByChild(20000);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->inhparent, 16384u);
    EXPECT_EQ(child->inhseqnum, 1);

    auto children = catalog_->GetInheritsByParent(16384);
    ASSERT_EQ(children.size(), 1u);
    EXPECT_EQ(children[0]->inhrelid, 20000u);
}

TEST_F(CatalogP06Test, InheritsDelete) {
    auto* r = makePallocNode<FormData_pg_inherits>();
    r->inhrelid = 20000;
    r->inhparent = 16384;
    catalog_->InsertInherits(r);

    EXPECT_TRUE(catalog_->DeleteInherits(20000));
    EXPECT_EQ(catalog_->GetInheritsByChild(20000), nullptr);
    EXPECT_FALSE(catalog_->DeleteInherits(20000));
}

// ===========================================================================
// pg_am CRUD
// ===========================================================================

TEST_F(CatalogP06Test, AmInsertAndGet) {
    auto* r = makePallocNode<FormData_pg_am>();
    r->amname = "btree";
    r->amtype = AmType::kIndex;
    r->amcanorder = true;
    r->amcanunique = true;
    Oid oid = catalog_->InsertAm(r);

    const FormData_pg_am* got = catalog_->GetAmByOid(oid);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->amname, "btree");
    EXPECT_EQ(got->amtype, AmType::kIndex);
    EXPECT_TRUE(got->amcanorder);
    EXPECT_TRUE(got->amcanunique);

    const FormData_pg_am* got2 = catalog_->GetAmByName("btree");
    ASSERT_NE(got2, nullptr);
    EXPECT_EQ(got2->oid, oid);
}

TEST_F(CatalogP06Test, AmGetMissingReturnsNull) {
    EXPECT_EQ(catalog_->GetAmByOid(99999), nullptr);
    EXPECT_EQ(catalog_->GetAmByName("nonexistent"), nullptr);
}

// ===========================================================================
// pg_tablespace CRUD
// ===========================================================================

TEST_F(CatalogP06Test, TablespaceInsertAndGet) {
    auto* r = makePallocNode<FormData_pg_tablespace>();
    r->spcname = "ts1";
    r->spcowner = 10;
    r->spclocation = "/data/ts1";
    Oid oid = catalog_->InsertTablespace(r);

    const FormData_pg_tablespace* got = catalog_->GetTablespaceByOid(oid);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->spcname, "ts1");
    EXPECT_EQ(got->spclocation, "/data/ts1");

    ASSERT_NE(catalog_->GetTablespaceByName("ts1"), nullptr);
    EXPECT_EQ(catalog_->GetTablespaceByName("ts1")->spcowner, 10u);
}

TEST_F(CatalogP06Test, TablespaceDelete) {
    auto* r = makePallocNode<FormData_pg_tablespace>();
    r->spcname = "ts1";
    Oid oid = catalog_->InsertTablespace(r);
    EXPECT_TRUE(catalog_->DeleteTablespace(oid));
    EXPECT_EQ(catalog_->GetTablespaceByOid(oid), nullptr);
}

// ===========================================================================
// pg_trigger CRUD
// ===========================================================================

TEST_F(CatalogP06Test, TriggerInsertAndGet) {
    auto* r = makePallocNode<FormData_pg_trigger>();
    r->tgname = "trg_before_insert";
    r->tgrelid = 20000;
    r->tgfoid = 20001;
    r->tgenabled = TriggerEnabled::kAlways;
    r->tgdeferrable = true;
    r->tgattr = {1, 2};
    Oid oid = catalog_->InsertTrigger(r);

    const FormData_pg_trigger* got = catalog_->GetTriggerByOid(oid);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->tgname, "trg_before_insert");
    EXPECT_EQ(got->tgrelid, 20000u);
    EXPECT_EQ(got->tgenabled, TriggerEnabled::kAlways);
    EXPECT_TRUE(got->tgdeferrable);
    ASSERT_EQ(got->tgattr.size(), 2u);
}

TEST_F(CatalogP06Test, TriggerGetByRelidAndDelete) {
    catalog_->InsertTrigger([&] {
        auto* r = makePallocNode<FormData_pg_trigger>();
        r->tgrelid = 20000;
        r->tgname = "t1";
        return r;
    }());
    catalog_->InsertTrigger([&] {
        auto* r = makePallocNode<FormData_pg_trigger>();
        r->tgrelid = 20000;
        r->tgname = "t2";
        return r;
    }());

    EXPECT_EQ(catalog_->GetTriggersByRelid(20000).size(), 2u);
    EXPECT_EQ(catalog_->DeleteTriggersForRelid(20000), 2u);
    EXPECT_EQ(catalog_->GetTriggersByRelid(20000).size(), 0u);
}

// ===========================================================================
// pg_rewrite CRUD
// ===========================================================================

TEST_F(CatalogP06Test, RewriteInsertAndGet) {
    auto* r = makePallocNode<FormData_pg_rewrite>();
    r->rulename = "_RETURN";
    r->ev_class = 20000;  // view OID
    r->ev_type = '1';     // SELECT
    r->is_instead = true;
    Oid oid = catalog_->InsertRewrite(r);

    const FormData_pg_rewrite* got = catalog_->GetRewriteByOid(oid);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->rulename, "_RETURN");
    EXPECT_EQ(got->ev_class, 20000u);
    EXPECT_EQ(got->ev_type, '1');
    EXPECT_TRUE(got->is_instead);
}

TEST_F(CatalogP06Test, RewriteGetByRelidAndDelete) {
    catalog_->InsertRewrite([&] {
        auto* r = makePallocNode<FormData_pg_rewrite>();
        r->ev_class = 20000;
        return r;
    }());

    EXPECT_EQ(catalog_->GetRewritesByRelid(20000).size(), 1u);
    EXPECT_EQ(catalog_->DeleteRewritesForRelid(20000), 1u);
    EXPECT_EQ(catalog_->GetRewritesByRelid(20000).size(), 0u);
}

// ===========================================================================
// Persistence: round-trip all 12 new tables through Save/Load.
// ===========================================================================

TEST_F(CatalogP06Test, SaveLoadRoundTripAllNewTables) {
    // Populate one row in each new table with non-default fields.
    auto* ns = makePallocNode<FormData_pg_namespace>();
    ns->nspname = "roundtrip_nsp";
    ns->nspowner = 10;
    ns->nspacl = true;
    catalog_->InsertNamespace(ns);

    auto* db = makePallocNode<FormData_pg_database>();
    db->datname = "roundtrip_db";
    db->encoding = 6;
    db->datcollate = "en_US.UTF-8";
    db->datconnlimit = 50;
    db->datfrozenxid = 1000;
    catalog_->InsertDatabase(db);

    auto* idx = makePallocNode<FormData_pg_index>();
    idx->indrelid = 20000;
    idx->indnatts = 2;
    idx->indnkeyatts = 2;
    idx->indisunique = true;
    idx->indisprimary = true;
    idx->indkey = {1, 2};
    idx->indcollation = {100, 100};
    idx->indclass = {1979, 1979};
    idx->indoption = {0, 1};
    catalog_->InsertIndex(idx);

    auto* con = makePallocNode<FormData_pg_constraint>();
    con->conname = "roundtrip_fk";
    con->contype = ConstraintType::kForeignKey;
    con->conrelid = 20000;
    con->conkey = {1};
    con->confkey = {2};
    con->conpfeqop = {4104};
    con->confdeltype = ConstraintAction::kCascade;
    con->confmatchtype = ConstraintMatch::kFull;
    con->convalidated = true;
    catalog_->InsertConstraint(con);

    auto* ad = makePallocNode<FormData_pg_attrdef>();
    ad->adrelid = 20000;
    ad->adnum = 1;
    ad->adsrc = "0";
    catalog_->InsertAttrdef(ad);

    auto* dep = makePallocNode<FormData_pg_depend>();
    dep->classid = 1259;
    dep->objid = 20000;
    dep->refclassid = 1259;
    dep->refobjid = 20001;
    dep->deptype = DependencyType::kAuto;
    catalog_->InsertDepend(dep);

    auto* st = makePallocNode<FormData_pg_statistic>();
    st->starelid = 20000;
    st->staattnum = 1;
    st->stanullfrac = 0.5F;
    st->stawidth = 4;
    st->stadistinct = 100;
    st->stakind1 = 1;
    st->stavalues1 = "a,b,c";
    catalog_->InsertStatistic(st);

    auto* inh = makePallocNode<FormData_pg_inherits>();
    inh->inhrelid = 20002;
    inh->inhparent = 20000;
    inh->inhseqnum = 1;
    catalog_->InsertInherits(inh);

    auto* am = makePallocNode<FormData_pg_am>();
    am->amname = "roundtrip_am";
    am->amtype = AmType::kIndex;
    am->amcanorder = true;
    am->amcanmulticol = true;
    catalog_->InsertAm(am);

    auto* spc = makePallocNode<FormData_pg_tablespace>();
    spc->spcname = "roundtrip_ts";
    spc->spcowner = 10;
    spc->spclocation = "/data/ts";
    spc->spcmaxsize = 1024;
    catalog_->InsertTablespace(spc);

    auto* trg = makePallocNode<FormData_pg_trigger>();
    trg->tgname = "roundtrip_trg";
    trg->tgrelid = 20000;
    trg->tgfoid = 20001;
    trg->tgenabled = TriggerEnabled::kReplica;
    trg->tgattr = {1, 3};
    trg->tgnewtable = "newtab";
    catalog_->InsertTrigger(trg);

    auto* rw = makePallocNode<FormData_pg_rewrite>();
    rw->rulename = "_RETURN";
    rw->ev_class = 20000;
    rw->ev_type = '1';
    rw->is_instead = true;
    catalog_->InsertRewrite(rw);

    // Snapshot OIDs before save for comparison.
    Oid ns_oid = ns->oid;
    Oid db_oid = db->oid;
    Oid idx_oid = idx->indexrelid;
    Oid con_oid = con->oid;
    Oid ad_oid = ad->oid;
    Oid am_oid = am->oid;
    Oid spc_oid = spc->oid;
    Oid trg_oid = trg->oid;
    Oid rw_oid = rw->oid;
    // 9 of the 12 inserts auto-allocate an OID (pg_depend, pg_statistic,
    // and pg_inherits do not), so next_oid_ advanced from kFirstNormalObjectId
    // (16384) to kFirstNormalObjectId + 9 (16393). Save/Load must preserve it.

    std::string path = TmpPath();
    ASSERT_TRUE(catalog_->Save(path));

    Catalog cat2;
    ASSERT_TRUE(cat2.Load(path));

    // pg_namespace
    const FormData_pg_namespace* ns2 = cat2.GetNamespaceByOid(ns_oid);
    ASSERT_NE(ns2, nullptr);
    EXPECT_EQ(ns2->nspname, "roundtrip_nsp");
    EXPECT_EQ(ns2->nspowner, 10u);
    EXPECT_TRUE(ns2->nspacl);

    // pg_database
    const FormData_pg_database* db2 = cat2.GetDatabaseByOid(db_oid);
    ASSERT_NE(db2, nullptr);
    EXPECT_EQ(db2->datname, "roundtrip_db");
    EXPECT_EQ(db2->encoding, 6);
    EXPECT_EQ(db2->datcollate, "en_US.UTF-8");
    EXPECT_EQ(db2->datconnlimit, 50);
    EXPECT_EQ(db2->datfrozenxid, 1000u);

    // pg_index (vector fields must round-trip)
    const FormData_pg_index* idx2 = cat2.GetIndexByOid(idx_oid);
    ASSERT_NE(idx2, nullptr);
    EXPECT_EQ(idx2->indrelid, 20000u);
    EXPECT_EQ(idx2->indnatts, 2);
    EXPECT_TRUE(idx2->indisunique);
    EXPECT_TRUE(idx2->indisprimary);
    ASSERT_EQ(idx2->indkey.size(), 2u);
    EXPECT_EQ(idx2->indkey[0], 1);
    EXPECT_EQ(idx2->indkey[1], 2);
    ASSERT_EQ(idx2->indcollation.size(), 2u);
    EXPECT_EQ(idx2->indcollation[0], 100u);
    ASSERT_EQ(idx2->indclass.size(), 2u);
    EXPECT_EQ(idx2->indclass[1], 1979u);
    ASSERT_EQ(idx2->indoption.size(), 2u);
    EXPECT_EQ(idx2->indoption[1], 1);

    // pg_constraint (enum + vector fields)
    const FormData_pg_constraint* con2 = cat2.GetConstraintByOid(con_oid);
    ASSERT_NE(con2, nullptr);
    EXPECT_EQ(con2->conname, "roundtrip_fk");
    EXPECT_EQ(con2->contype, ConstraintType::kForeignKey);
    EXPECT_EQ(con2->confdeltype, ConstraintAction::kCascade);
    EXPECT_EQ(con2->confmatchtype, ConstraintMatch::kFull);
    EXPECT_TRUE(con2->convalidated);
    ASSERT_EQ(con2->conkey.size(), 1u);
    ASSERT_EQ(con2->confkey.size(), 1u);
    EXPECT_EQ(con2->confkey[0], 2);
    ASSERT_EQ(con2->conpfeqop.size(), 1u);
    EXPECT_EQ(con2->conpfeqop[0], 4104u);

    // pg_attrdef
    const FormData_pg_attrdef* ad2 = cat2.GetAttrdef(20000, 1);
    ASSERT_NE(ad2, nullptr);
    EXPECT_EQ(ad2->oid, ad_oid);
    EXPECT_EQ(ad2->adsrc, "0");

    // pg_depend
    auto deps = cat2.GetDependsByObj(1259, 20000);
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0]->refobjid, 20001u);
    EXPECT_EQ(deps[0]->deptype, DependencyType::kAuto);

    // pg_statistic (float + int + string fields)
    const FormData_pg_statistic* st2 = cat2.GetStatistic(20000, 1);
    ASSERT_NE(st2, nullptr);
    EXPECT_FLOAT_EQ(st2->stanullfrac, 0.5F);
    EXPECT_EQ(st2->stawidth, 4);
    EXPECT_EQ(st2->stadistinct, 100);
    EXPECT_EQ(st2->stakind1, 1);
    EXPECT_EQ(st2->stavalues1, "a,b,c");

    // pg_inherits
    const FormData_pg_inherits* inh2 = cat2.GetInheritsByChild(20002);
    ASSERT_NE(inh2, nullptr);
    EXPECT_EQ(inh2->inhparent, 20000u);
    EXPECT_EQ(inh2->inhseqnum, 1);
    EXPECT_EQ(cat2.GetInheritsByParent(20000).size(), 1u);

    // pg_am
    const FormData_pg_am* am2 = cat2.GetAmByOid(am_oid);
    ASSERT_NE(am2, nullptr);
    EXPECT_EQ(am2->amname, "roundtrip_am");
    EXPECT_EQ(am2->amtype, AmType::kIndex);
    EXPECT_TRUE(am2->amcanorder);
    EXPECT_TRUE(am2->amcanmulticol);

    // pg_tablespace
    const FormData_pg_tablespace* spc2 = cat2.GetTablespaceByOid(spc_oid);
    ASSERT_NE(spc2, nullptr);
    EXPECT_EQ(spc2->spcname, "roundtrip_ts");
    EXPECT_EQ(spc2->spclocation, "/data/ts");
    EXPECT_EQ(spc2->spcmaxsize, 1024);

    // pg_trigger
    const FormData_pg_trigger* trg2 = cat2.GetTriggerByOid(trg_oid);
    ASSERT_NE(trg2, nullptr);
    EXPECT_EQ(trg2->tgname, "roundtrip_trg");
    EXPECT_EQ(trg2->tgrelid, 20000u);
    EXPECT_EQ(trg2->tgenabled, TriggerEnabled::kReplica);
    ASSERT_EQ(trg2->tgattr.size(), 2u);
    EXPECT_EQ(trg2->tgattr[0], 1);
    EXPECT_EQ(trg2->tgattr[1], 3);
    EXPECT_EQ(trg2->tgnewtable, "newtab");

    // pg_rewrite
    const FormData_pg_rewrite* rw2 = cat2.GetRewriteByOid(rw_oid);
    ASSERT_NE(rw2, nullptr);
    EXPECT_EQ(rw2->rulename, "_RETURN");
    EXPECT_EQ(rw2->ev_class, 20000u);
    EXPECT_EQ(rw2->ev_type, '1');
    EXPECT_TRUE(rw2->is_instead);

    // next_oid_ restored: 9 auto-allocating inserts advanced it to
    // kFirstNormalObjectId + 9.
    EXPECT_EQ(cat2.AllocateOid(), kFirstNormalObjectId + 9);

    std::remove(path.c_str());
}

// ===========================================================================
// Persistence: built-in rows (oid < kFirstNormalObjectId) are not persisted
// for the new tables either.
// ===========================================================================

TEST_F(CatalogP06Test, SaveOmitsBuiltinRowsForNewTables) {
    auto* builtin_am = makePallocNode<FormData_pg_am>();
    builtin_am->amname = "btree";
    builtin_am->oid = 403;  // builtin OID
    catalog_->InsertAm(builtin_am);

    auto* user_am = makePallocNode<FormData_pg_am>();
    user_am->amname = "user_am";
    // oid auto-assigned -> >= kFirstNormalObjectId
    Oid user_oid = catalog_->InsertAm(user_am);

    std::string path = TmpPath();
    ASSERT_TRUE(catalog_->Save(path));

    Catalog cat2;
    ASSERT_TRUE(cat2.Load(path));

    EXPECT_EQ(cat2.GetAmByOid(403), nullptr);
    ASSERT_NE(cat2.GetAmByOid(user_oid), nullptr);
    EXPECT_EQ(cat2.GetAmByName("user_am")->oid, user_oid);

    std::remove(path.c_str());
}

}  // namespace
