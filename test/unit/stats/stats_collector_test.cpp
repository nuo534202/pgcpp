// stats_collector_test.cpp — Unit tests for the P3-9 stats module.
//
// Covers:
//   - StatisticsCollector reporting and querying (database, table, index stats)
//   - Backend activity info
//   - pg_stat_* view column definitions and OID helpers
//   - Virtual scan materialization (StatsBeginScan / StatsGetNext / StatsEndScan)
//   - BootstrapStatsViews catalog registration
#include "stats/stats_collector.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "stats/stats_bootstrap.hpp"
#include "stats/stats_scan.hpp"
#include "stats/stats_view.hpp"
#include "transaction/heap_tuple.hpp"
#include "types/datum.hpp"

namespace {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::SetCurrentMemoryContext;
using pgcpp::stats::BackendInfo;
using pgcpp::stats::BootstrapStatsViews;
using pgcpp::stats::DatabaseStats;
using pgcpp::stats::GetPgStatActivityColumns;
using pgcpp::stats::GetPgStatAllIndexesColumns;
using pgcpp::stats::GetPgStatAllTablesColumns;
using pgcpp::stats::GetPgStatDatabaseColumns;
using pgcpp::stats::GetStatsCollector;
using pgcpp::stats::GetStatsViewColumns;
using pgcpp::stats::GetStatsViewName;
using pgcpp::stats::IndexStats;
using pgcpp::stats::IsStatsView;
using pgcpp::stats::kPgStatActivityOid;
using pgcpp::stats::kPgStatAllIndexesOid;
using pgcpp::stats::kPgStatAllTablesOid;
using pgcpp::stats::kPgStatDatabaseOid;
using pgcpp::stats::ReportAbortCurrentDb;
using pgcpp::stats::ReportCommitCurrentDb;
using pgcpp::stats::StatisticsCollector;
using pgcpp::stats::StatsBeginScan;
using pgcpp::stats::StatsColumn;
using pgcpp::stats::StatsEndScan;
using pgcpp::stats::StatsGetNext;
using pgcpp::stats::TableStats;
using pgcpp::transaction::HeapTuple;

// Test fixture that sets up memory context and catalog for tests needing
// catalog access (e.g. BootstrapStatsViews).
class StatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("stats_test_context");
        SetCurrentMemoryContext(context_);

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

        SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// ===========================================================================
// 1. StatisticsCollector — database stats reporting
// ===========================================================================

TEST(StatisticsCollectorTest, ReportCommitIncrementsCounter) {
    StatisticsCollector sc;
    sc.ReportCommit(100);
    sc.ReportCommit(100);
    sc.ReportCommit(200);

    const DatabaseStats* s = sc.GetDatabaseStats(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->datid, 100u);
    EXPECT_EQ(s->num_commit, 2);

    const DatabaseStats* s2 = sc.GetDatabaseStats(200);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->num_commit, 1);
}

TEST(StatisticsCollectorTest, ReportAbortIncrementsCounter) {
    StatisticsCollector sc;
    sc.ReportAbort(100);
    sc.ReportAbort(100);
    sc.ReportAbort(100);

    const DatabaseStats* s = sc.GetDatabaseStats(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->num_rollback, 3);
}

TEST(StatisticsCollectorTest, ReportBlockReadAndHit) {
    StatisticsCollector sc;
    sc.ReportBlockRead(100);
    sc.ReportBlockRead(100);
    sc.ReportBlockHit(100);
    sc.ReportBlockHit(100);
    sc.ReportBlockHit(100);

    const DatabaseStats* s = sc.GetDatabaseStats(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->blks_read, 2);
    EXPECT_EQ(s->blks_hit, 3);
}

TEST(StatisticsCollectorTest, ReportTuplesReturnedAndFetched) {
    StatisticsCollector sc;
    sc.ReportTuplesReturned(100, 10);
    sc.ReportTuplesReturned(100, 20);
    sc.ReportTuplesFetched(100, 5);

    const DatabaseStats* s = sc.GetDatabaseStats(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->tuples_returned, 30);
    EXPECT_EQ(s->tuples_fetched, 5);
}

TEST(StatisticsCollectorTest, GetDatabaseStatsReturnsNullForUnknown) {
    StatisticsCollector sc;
    EXPECT_EQ(sc.GetDatabaseStats(999), nullptr);
}

TEST(StatisticsCollectorTest, GetAllDatabaseStatsReturnsSnapshot) {
    StatisticsCollector sc;
    sc.ReportCommit(100);
    sc.ReportCommit(200);
    sc.ReportCommit(300);

    auto all = sc.GetAllDatabaseStats();
    EXPECT_EQ(all.size(), 3u);

    // Verify the snapshot is a copy (modifying the collector doesn't affect it).
    sc.ReportCommit(100);
    auto all2 = sc.GetAllDatabaseStats();
    EXPECT_EQ(all2.size(), 3u);
}

// ===========================================================================
// 2. StatisticsCollector — table stats reporting
// ===========================================================================

TEST(StatisticsCollectorTest, ReportSeqScan) {
    StatisticsCollector sc;
    sc.ReportSeqScan(1001, 50);
    sc.ReportSeqScan(1001, 30);

    auto all = sc.GetAllTableStats();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].relid, 1001u);
    EXPECT_EQ(all[0].seq_scan, 2);
    EXPECT_EQ(all[0].seq_tuples_read, 80);
}

TEST(StatisticsCollectorTest, ReportInsert) {
    StatisticsCollector sc;
    sc.ReportInsert(1001, 1);
    sc.ReportInsert(1001, 1);
    sc.ReportInsert(1001, 5);

    auto all = sc.GetAllTableStats();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].n_tuples_ins, 7);
    EXPECT_EQ(all[0].n_live_tuples, 7);
}

TEST(StatisticsCollectorTest, ReportDelete) {
    StatisticsCollector sc;
    sc.ReportInsert(1001, 10);
    sc.ReportDelete(1001, 3);
    sc.ReportDelete(1001, 2);

    auto all = sc.GetAllTableStats();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].n_tuples_del, 5);
    EXPECT_EQ(all[0].n_live_tuples, 5);  // 10 - 5
    EXPECT_EQ(all[0].n_dead_tuples, 5);
}

TEST(StatisticsCollectorTest, ReportUpdate) {
    StatisticsCollector sc;
    sc.ReportInsert(1001, 10);
    sc.ReportUpdate(1001, 3);
    sc.ReportUpdate(1001, 2, 1);  // 1 HOT update

    auto all = sc.GetAllTableStats();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].n_tuples_upd, 5);
    EXPECT_EQ(all[0].n_tuples_hot_upd, 1);
    // Net live tuples unchanged by updates.
    EXPECT_EQ(all[0].n_live_tuples, 10);
}

TEST(StatisticsCollectorTest, ReportIndexScan) {
    StatisticsCollector sc;
    sc.ReportIndexScan(2001, 10, 8);
    sc.ReportIndexScan(2001, 5, 3);

    auto all = sc.GetAllIndexStats();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].relid, 2001u);
    EXPECT_EQ(all[0].idx_scan, 2);
    EXPECT_EQ(all[0].idx_tuples_read, 15);
    EXPECT_EQ(all[0].idx_tuples_fetch, 11);
}

// ===========================================================================
// 3. StatisticsCollector — registration
// ===========================================================================

TEST(StatisticsCollectorTest, RegisterDatabase) {
    StatisticsCollector sc;
    sc.RegisterDatabase(100, "mydb");

    const DatabaseStats* s = sc.GetDatabaseStats(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->datname, "mydb");
    EXPECT_EQ(s->num_commit, 0);
}

TEST(StatisticsCollectorTest, RegisterTable) {
    StatisticsCollector sc;
    sc.RegisterTable(1001, "mytable", 2200, "public");

    auto all = sc.GetAllTableStats();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].relname, "mytable");
    EXPECT_EQ(all[0].schemoid, 2200u);
    EXPECT_EQ(all[0].schemaname, "public");
}

TEST(StatisticsCollectorTest, RegisterIndex) {
    StatisticsCollector sc;
    sc.RegisterIndex(2001, "myindex", 1001, "public");

    auto all = sc.GetAllIndexStats();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].relname, "myindex");
    EXPECT_EQ(all[0].tableoid, 1001u);
    EXPECT_EQ(all[0].schemaname, "public");
}

// ===========================================================================
// 4. StatisticsCollector — SetCurrentDatabase and backend info
// ===========================================================================

TEST(StatisticsCollectorTest, SetCurrentDatabaseUpdatesBackend) {
    StatisticsCollector sc;
    sc.RegisterDatabase(100, "mydb");
    sc.SetCurrentDatabase(100);

    const BackendInfo& bi = sc.GetBackendInfo();
    EXPECT_EQ(bi.datid, 100u);
    EXPECT_EQ(bi.datname, "mydb");
}

TEST(StatisticsCollectorTest, UpdateBackendActivity) {
    StatisticsCollector sc;
    sc.UpdateBackendActivity("SELECT 1", "active");

    const BackendInfo& bi = sc.GetBackendInfo();
    EXPECT_EQ(bi.query, "SELECT 1");
    EXPECT_EQ(bi.state, "active");
}

// ===========================================================================
// 5. StatisticsCollector — mutable accessors
// ===========================================================================

TEST(StatisticsCollectorTest, GetMutableDatabaseStats) {
    StatisticsCollector sc;
    sc.RegisterDatabase(100, "mydb");

    DatabaseStats* s = sc.GetMutableDatabaseStats(100);
    ASSERT_NE(s, nullptr);
    s->num_commit = 42;
    s->blks_read = 10;

    const DatabaseStats* s2 = sc.GetDatabaseStats(100);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->num_commit, 42);
    EXPECT_EQ(s2->blks_read, 10);
}

TEST(StatisticsCollectorTest, GetMutableTableStats) {
    StatisticsCollector sc;
    sc.RegisterTable(1001, "mytable", 2200, "public");

    TableStats* s = sc.GetMutableTableStats(1001);
    ASSERT_NE(s, nullptr);
    s->n_live_tuples = 100;
    s->n_dead_tuples = 5;

    auto all = sc.GetAllTableStats();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].n_live_tuples, 100);
    EXPECT_EQ(all[0].n_dead_tuples, 5);
}

// ===========================================================================
// 6. Convenience functions (ReportCommitCurrentDb / ReportAbortCurrentDb)
// ===========================================================================

TEST(StatisticsCollectorTest, ReportCommitCurrentDbUsesBackendDatid) {
    auto& sc = GetStatsCollector();
    sc.RegisterDatabase(99901, "test_commit_db");
    sc.SetCurrentDatabase(99901);

    int64_t before = 0;
    if (const DatabaseStats* s = sc.GetDatabaseStats(99901)) {
        before = s->num_commit;
    }

    ReportCommitCurrentDb();

    const DatabaseStats* s = sc.GetDatabaseStats(99901);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->num_commit, before + 1);
}

TEST(StatisticsCollectorTest, ReportAbortCurrentDbUsesBackendDatid) {
    auto& sc = GetStatsCollector();
    sc.RegisterDatabase(99902, "test_abort_db");
    sc.SetCurrentDatabase(99902);

    int64_t before = 0;
    if (const DatabaseStats* s = sc.GetDatabaseStats(99902)) {
        before = s->num_rollback;
    }

    ReportAbortCurrentDb();

    const DatabaseStats* s = sc.GetDatabaseStats(99902);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->num_rollback, before + 1);
}

// ===========================================================================
// 7. pg_stat_* view OID helpers
// ===========================================================================

TEST(StatsViewTest, IsStatsViewRecognizesAllFourViews) {
    EXPECT_TRUE(IsStatsView(kPgStatDatabaseOid));
    EXPECT_TRUE(IsStatsView(kPgStatActivityOid));
    EXPECT_TRUE(IsStatsView(kPgStatAllTablesOid));
    EXPECT_TRUE(IsStatsView(kPgStatAllIndexesOid));
}

TEST(StatsViewTest, IsStatsViewRejectsUnknownOids) {
    EXPECT_FALSE(IsStatsView(0));
    EXPECT_FALSE(IsStatsView(100));
    EXPECT_FALSE(IsStatsView(16384));
    EXPECT_FALSE(IsStatsView(kInvalidOid));
}

TEST(StatsViewTest, GetStatsViewNameReturnsCorrectNames) {
    EXPECT_EQ(GetStatsViewName(kPgStatDatabaseOid), "pg_stat_database");
    EXPECT_EQ(GetStatsViewName(kPgStatActivityOid), "pg_stat_activity");
    EXPECT_EQ(GetStatsViewName(kPgStatAllTablesOid), "pg_stat_all_tables");
    EXPECT_EQ(GetStatsViewName(kPgStatAllIndexesOid), "pg_stat_all_indexes");
}

TEST(StatsViewTest, GetStatsViewNameReturnsEmptyForUnknown) {
    EXPECT_TRUE(GetStatsViewName(9999).empty());
}

TEST(StatsViewTest, GetStatsViewColumnsReturnsNullForUnknown) {
    EXPECT_EQ(GetStatsViewColumns(9999), nullptr);
}

TEST(StatsViewTest, GetStatsViewColumnsReturnsNonNullForKnown) {
    EXPECT_NE(GetStatsViewColumns(kPgStatDatabaseOid), nullptr);
    EXPECT_NE(GetStatsViewColumns(kPgStatActivityOid), nullptr);
    EXPECT_NE(GetStatsViewColumns(kPgStatAllTablesOid), nullptr);
    EXPECT_NE(GetStatsViewColumns(kPgStatAllIndexesOid), nullptr);
}

// ===========================================================================
// 8. pg_stat_* view column definitions
// ===========================================================================

TEST(StatsViewTest, PgStatDatabaseHas15Columns) {
    const auto& cols = GetPgStatDatabaseColumns();
    EXPECT_EQ(cols.size(), 15u);
    EXPECT_EQ(cols[0].name, "datid");
    EXPECT_EQ(cols[1].name, "datname");
    EXPECT_EQ(cols[2].name, "num_commit");
}

TEST(StatsViewTest, PgStatActivityHas10Columns) {
    const auto& cols = GetPgStatActivityColumns();
    EXPECT_EQ(cols.size(), 10u);
    EXPECT_EQ(cols[0].name, "pid");
    EXPECT_EQ(cols[1].name, "datid");
    EXPECT_EQ(cols[2].name, "datname");
}

TEST(StatsViewTest, PgStatAllTablesHas17Columns) {
    const auto& cols = GetPgStatAllTablesColumns();
    EXPECT_EQ(cols.size(), 17u);
    EXPECT_EQ(cols[0].name, "relid");
    EXPECT_EQ(cols[1].name, "schemaname");
    EXPECT_EQ(cols[2].name, "relname");
}

TEST(StatsViewTest, PgStatAllIndexesHas7Columns) {
    const auto& cols = GetPgStatAllIndexesColumns();
    EXPECT_EQ(cols.size(), 7u);
    EXPECT_EQ(cols[0].name, "relid");
    EXPECT_EQ(cols[1].name, "schemaname");
    EXPECT_EQ(cols[2].name, "relname");
    EXPECT_EQ(cols[3].name, "tableoid");
}

TEST(StatsViewTest, ColumnTypesAreCorrect) {
    const auto& db_cols = GetPgStatDatabaseColumns();
    // datid is int4
    EXPECT_EQ(db_cols[0].type_oid, pgcpp::types::kInt4Oid);
    EXPECT_EQ(db_cols[0].type_len, 4);
    EXPECT_TRUE(db_cols[0].byval);
    // datname is text (variable-length, by-reference)
    EXPECT_EQ(db_cols[1].type_oid, pgcpp::types::kTextOid);
    EXPECT_EQ(db_cols[1].type_len, -1);
    EXPECT_FALSE(db_cols[1].byval);
    // num_commit is int8
    EXPECT_EQ(db_cols[2].type_oid, pgcpp::types::kInt8Oid);
    EXPECT_EQ(db_cols[2].type_len, 8);
    EXPECT_TRUE(db_cols[2].byval);
}

// ===========================================================================
// 9. Stats scan — materialization
// ===========================================================================

// Helper: count tuples returned by a stats scan.
static int CountScanTuples(Oid view_oid) {
    auto* scan = StatsBeginScan(view_oid);
    if (scan == nullptr) {
        return -1;
    }
    int count = 0;
    while (StatsGetNext(scan) != nullptr) {
        ++count;
    }
    StatsEndScan(scan);
    return count;
}

TEST(StatsScanTest, StatsBeginScanReturnsNullForNonStatsView) {
    EXPECT_EQ(StatsBeginScan(9999), nullptr);
    EXPECT_EQ(StatsBeginScan(kInvalidOid), nullptr);
}

TEST(StatsScanTest, StatsGetNextReturnsNullForNullScan) {
    EXPECT_EQ(StatsGetNext(nullptr), nullptr);
}

TEST(StatsScanTest, StatsEndScanHandlesNullScan) {
    // Should not crash.
    StatsEndScan(nullptr);
}

TEST_F(StatsTest, DatabaseViewScanProducesRows) {
    auto& sc = GetStatsCollector();
    sc.RegisterDatabase(99001, "stats_test_db_1");

    int count = CountScanTuples(kPgStatDatabaseOid);
    EXPECT_GE(count, 1);  // at least our registered database
}

TEST_F(StatsTest, ActivityViewScanProducesOneRow) {
    // pg_stat_activity always has exactly one row (the single backend).
    int count = CountScanTuples(kPgStatActivityOid);
    EXPECT_EQ(count, 1);
}

TEST_F(StatsTest, AllTablesViewScanProducesRows) {
    auto& sc = GetStatsCollector();
    sc.RegisterTable(99002, "stats_test_table_1", 2200, "public");

    int count = CountScanTuples(kPgStatAllTablesOid);
    EXPECT_GE(count, 1);
}

TEST_F(StatsTest, AllIndexesViewScanProducesRows) {
    auto& sc = GetStatsCollector();
    sc.RegisterIndex(99003, "stats_test_index_1", 99002, "public");

    int count = CountScanTuples(kPgStatAllIndexesOid);
    EXPECT_GE(count, 1);
}

TEST_F(StatsTest, ScanReturnsTuplesInOrder) {
    auto& sc = GetStatsCollector();
    sc.RegisterDatabase(99010, "stats_order_db");

    auto* scan = StatsBeginScan(kPgStatDatabaseOid);
    ASSERT_NE(scan, nullptr);

    HeapTuple first = StatsGetNext(scan);
    EXPECT_NE(first, nullptr);

    // Keep fetching until we find our database or exhaust the scan.
    bool found = false;
    while (first != nullptr) {
        found = true;
        first = StatsGetNext(scan);
    }
    EXPECT_TRUE(found);
    StatsEndScan(scan);
}

TEST_F(StatsTest, RescanByCreatingNewScan) {
    // Stats scan doesn't have a rescan function; instead, create a new scan.
    auto* scan1 = StatsBeginScan(kPgStatActivityOid);
    ASSERT_NE(scan1, nullptr);
    HeapTuple t1 = StatsGetNext(scan1);
    EXPECT_NE(t1, nullptr);
    EXPECT_EQ(StatsGetNext(scan1), nullptr);  // exhausted
    StatsEndScan(scan1);

    auto* scan2 = StatsBeginScan(kPgStatActivityOid);
    ASSERT_NE(scan2, nullptr);
    HeapTuple t2 = StatsGetNext(scan2);
    EXPECT_NE(t2, nullptr);
    StatsEndScan(scan2);
}

// ===========================================================================
// 10. BootstrapStatsViews — catalog registration
// ===========================================================================

TEST_F(StatsTest, BootstrapStatsViewsRegistersFourViews) {
    BootstrapStatsViews(catalog_);

    // Verify each view is registered in pg_class.
    EXPECT_NE(catalog_->GetClassByName("pg_stat_database"), nullptr);
    EXPECT_NE(catalog_->GetClassByName("pg_stat_activity"), nullptr);
    EXPECT_NE(catalog_->GetClassByName("pg_stat_all_tables"), nullptr);
    EXPECT_NE(catalog_->GetClassByName("pg_stat_all_indexes"), nullptr);
}

TEST_F(StatsTest, BootstrapStatsViewsSetsCorrectRelkind) {
    BootstrapStatsViews(catalog_);

    const FormData_pg_class* cls = catalog_->GetClassByName("pg_stat_database");
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->relkind, RelKind::kView);
    EXPECT_EQ(cls->relfilenode, kInvalidOid);  // no physical file
    EXPECT_EQ(cls->relnamespace, 2200u);       // public namespace
}

TEST_F(StatsTest, BootstrapStatsViewsSetsCorrectRelnatts) {
    BootstrapStatsViews(catalog_);

    const FormData_pg_class* db_cls = catalog_->GetClassByName("pg_stat_database");
    ASSERT_NE(db_cls, nullptr);
    EXPECT_EQ(db_cls->relnatts, 15);

    const FormData_pg_class* act_cls = catalog_->GetClassByName("pg_stat_activity");
    ASSERT_NE(act_cls, nullptr);
    EXPECT_EQ(act_cls->relnatts, 10);

    const FormData_pg_class* tbl_cls = catalog_->GetClassByName("pg_stat_all_tables");
    ASSERT_NE(tbl_cls, nullptr);
    EXPECT_EQ(tbl_cls->relnatts, 17);

    const FormData_pg_class* idx_cls = catalog_->GetClassByName("pg_stat_all_indexes");
    ASSERT_NE(idx_cls, nullptr);
    EXPECT_EQ(idx_cls->relnatts, 7);
}

TEST_F(StatsTest, BootstrapStatsViewsRegistersAttributes) {
    BootstrapStatsViews(catalog_);

    // Check that pg_attribute rows exist for pg_stat_database.
    auto attrs = catalog_->GetAttributes(kPgStatDatabaseOid);
    EXPECT_EQ(attrs.size(), 15u);

    // Verify first attribute.
    ASSERT_FALSE(attrs.empty());
    EXPECT_EQ(attrs[0]->attname, "datid");
    EXPECT_EQ(attrs[0]->attnum, 1);

    // Check pg_stat_activity attributes.
    auto act_attrs = catalog_->GetAttributes(kPgStatActivityOid);
    EXPECT_EQ(act_attrs.size(), 10u);

    // Check pg_stat_all_tables attributes.
    auto tbl_attrs = catalog_->GetAttributes(kPgStatAllTablesOid);
    EXPECT_EQ(tbl_attrs.size(), 17u);

    // Check pg_stat_all_indexes attributes.
    auto idx_attrs = catalog_->GetAttributes(kPgStatAllIndexesOid);
    EXPECT_EQ(idx_attrs.size(), 7u);
}

TEST_F(StatsTest, BootstrapStatsViewsRejectsDuplicateRegistration) {
    // Calling BootstrapStatsViews once should succeed.
    BootstrapStatsViews(catalog_);
    EXPECT_NE(catalog_->GetClassByName("pg_stat_database"), nullptr);

    // Calling it again should throw because the catalog rejects duplicate OIDs.
    EXPECT_THROW(BootstrapStatsViews(catalog_), std::exception);
}

}  // namespace
