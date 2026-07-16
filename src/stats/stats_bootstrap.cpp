// stats_bootstrap.cpp — Register pg_stat_* views in the catalog (P3-9).
//
// Registers the four pg_stat_* views as pg_class entries with relkind='v'
// (view) and relfilenode=kInvalidOid (no physical file). Their columns are
// registered in pg_attribute so the parser/executor can resolve column
// references. The executor detects stats view OIDs (via IsStatsView) and
// uses the virtual scan (stats_scan.cpp) instead of a heap scan.
//
// Called from BootstrapServer (server/bootstrap.cpp) after BootstrapCatalog.
#include "stats/stats_bootstrap.hpp"

#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"
#include "stats/stats_collector.hpp"
#include "stats/stats_view.hpp"

namespace pgcpp::stats {

using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::memory::palloc;
using pgcpp::nodes::makePallocNode;

namespace {

// Create and insert a pg_class row for a stats view.
void RegisterViewClass(pgcpp::catalog::Catalog* cat, Oid view_oid, const std::string& name,
                       int16_t natts) {
    auto* row = makePallocNode<FormData_pg_class>();
    row->oid = view_oid;
    row->relname = name;
    row->relnamespace = 2200;  // public namespace OID
    row->relkind = RelKind::kView;
    row->relpersistence = RelPersistence::kPermanent;
    row->relnatts = natts;
    row->relisshared = false;
    row->relhasindex = false;
    row->relhasrules = false;
    row->relhastriggers = false;
    row->relispopulated = true;
    row->relpages = 0;
    row->reltuples = 0.0F;
    cat->InsertClass(row);
}

// Create and insert pg_attribute rows for each column of a stats view.
void RegisterViewAttributes(pgcpp::catalog::Catalog* cat, Oid view_oid,
                            const std::vector<StatsColumn>& cols) {
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const StatsColumn& c = cols[i];
        auto* attr = makePallocNode<FormData_pg_attribute>();
        attr->attrelid = view_oid;
        attr->attname = c.name;
        attr->atttypid = c.type_oid;
        attr->attlen = c.type_len;
        attr->attnum = static_cast<int16_t>(i + 1);
        attr->attbyval = c.byval;
        attr->attcacheoff = -1;
        attr->atttypmod = -1;
        attr->attnotnull = false;
        attr->atthasdef = false;
        cat->InsertAttribute(attr);
    }
}

}  // namespace

void BootstrapStatsViews(pgcpp::catalog::Catalog* cat) {
    // pg_stat_database
    const auto& db_cols = GetPgStatDatabaseColumns();
    RegisterViewClass(cat, kPgStatDatabaseOid, "pg_stat_database",
                      static_cast<int16_t>(db_cols.size()));
    RegisterViewAttributes(cat, kPgStatDatabaseOid, db_cols);

    // pg_stat_activity
    const auto& act_cols = GetPgStatActivityColumns();
    RegisterViewClass(cat, kPgStatActivityOid, "pg_stat_activity",
                      static_cast<int16_t>(act_cols.size()));
    RegisterViewAttributes(cat, kPgStatActivityOid, act_cols);

    // pg_stat_all_tables
    const auto& tbl_cols = GetPgStatAllTablesColumns();
    RegisterViewClass(cat, kPgStatAllTablesOid, "pg_stat_all_tables",
                      static_cast<int16_t>(tbl_cols.size()));
    RegisterViewAttributes(cat, kPgStatAllTablesOid, tbl_cols);

    // pg_stat_all_indexes
    const auto& idx_cols = GetPgStatAllIndexesColumns();
    RegisterViewClass(cat, kPgStatAllIndexesOid, "pg_stat_all_indexes",
                      static_cast<int16_t>(idx_cols.size()));
    RegisterViewAttributes(cat, kPgStatAllIndexesOid, idx_cols);
}

}  // namespace pgcpp::stats
