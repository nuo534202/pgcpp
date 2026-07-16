// stats_bootstrap.hpp — Bootstrap pg_stat_* views in the catalog (P3-9).
#pragma once

namespace pgcpp::catalog {
class Catalog;
}  // namespace pgcpp::catalog

namespace pgcpp::stats {

// Register the four pg_stat_* views (pg_stat_database, pg_stat_activity,
// pg_stat_all_tables, pg_stat_all_indexes) as pg_class + pg_attribute rows
// in the given catalog. Called once during server bootstrap after
// BootstrapCatalog.
void BootstrapStatsViews(pgcpp::catalog::Catalog* cat);

}  // namespace pgcpp::stats
