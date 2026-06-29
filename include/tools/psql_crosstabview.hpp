// psql_crosstabview.h — Cross-tabulation (crosstabview.c).
//
// Converted from PostgreSQL 15's src/bin/psql/crosstabview.c.
//
// The `\crosstabview` meta-command re-displays the result of the just-run
// query as a pivot table: the values of one column become row headers, the
// values of another column become column headers, and a third column's
// values fill the cells.
//
// Example:
//   SELECT year, region, sales FROM ...
//   \crosstabview 1 2 3
//   produces:
//                  | north | south | east | west
//     2020         | 100   | 120   | 90   | 110
//     2021         | 105   | 130   | 95   | 115
//
// pgcpp provides a faithful implementation:
//   - The caller passes the QueryResult and the 1-based indices (or names)
//     of the row, column, and value columns.
//   - The output is written as an aligned table to a std::ostream.
#pragma once

#include <ostream>
#include <string>

#include "tools/psql_client.hpp"

namespace pgcpp::tools {

// CrosstabOptions — which columns to use for the pivot.
// Either index (1-based) or name can be set; name takes precedence.
struct CrosstabOptions {
    std::string row_col_name;
    std::string col_col_name;
    std::string value_col_name;
    int row_col_index = 1;
    int col_col_index = 2;
    int value_col_index = 3;
};

// CrosstabResult — outcome of a crosstab render.
enum class CrosstabResult {
    kOk,
    kNotEnoughColumns,  // result has fewer than 3 columns
    kInvalidColumn,     // specified column index/name out of range
};

// RenderCrosstab — render `result` as a pivot table to `out`.
// Returns kOk on success.
CrosstabResult RenderCrosstab(const QueryResult& result, const CrosstabOptions& opts,
                              std::ostream& out);

// ResolveColumnIndex — resolve a column specifier (name or 1-based index) to
// a 0-based index into result.column_names. Returns -1 if not found.
int ResolveColumnIndex(const QueryResult& result, const std::string& name, int fallback_index);

}  // namespace pgcpp::tools
