// psql_crosstabview.cpp — Cross-tabulation (crosstabview.c).
//
// Converted from PostgreSQL 15's src/bin/psql/crosstabview.c.
#include "tools/psql_crosstabview.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace pgcpp::tools {

namespace {

// Case-insensitive ASCII string equality. Used by ResolveColumnIndex to
// match column names the way PostgreSQL does (pg_strcasecmp semantics).
bool IEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

int ResolveColumnIndex(const QueryResult& result, const std::string& name, int fallback_index) {
    // If a name is given, search the column names case-insensitively.
    if (!name.empty()) {
        for (size_t i = 0; i < result.column_names.size(); ++i) {
            if (IEqual(result.column_names[i], name)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    // Otherwise treat fallback_index as 1-based and validate the range.
    int n = static_cast<int>(result.column_names.size());
    if (fallback_index >= 1 && fallback_index <= n) {
        return fallback_index - 1;
    }
    return -1;
}

CrosstabResult RenderCrosstab(const QueryResult& result, const CrosstabOptions& opts,
                              std::ostream& out) {
    if (result.column_names.size() < 3) {
        return CrosstabResult::kNotEnoughColumns;
    }

    int row_idx = ResolveColumnIndex(result, opts.row_col_name, opts.row_col_index);
    int col_idx = ResolveColumnIndex(result, opts.col_col_name, opts.col_col_index);
    int val_idx = ResolveColumnIndex(result, opts.value_col_name, opts.value_col_index);
    if (row_idx < 0 || col_idx < 0 || val_idx < 0) {
        return CrosstabResult::kInvalidColumn;
    }

    // Collect sorted distinct row/column header values and the cell map.
    // std::set/std::map iterate in lexicographic order, giving the sorted
    // order required by the spec. Last write wins for duplicate (row, col)
    // pairs, matching PostgreSQL's crosstabview behavior.
    std::set<std::string> row_set;
    std::set<std::string> col_set;
    std::map<std::string, std::map<std::string, std::string>> cells;

    for (const auto& row : result.rows) {
        if (row_idx >= static_cast<int>(row.size()))
            continue;
        if (col_idx >= static_cast<int>(row.size()))
            continue;
        if (val_idx >= static_cast<int>(row.size()))
            continue;
        std::string rv = row[row_idx];
        std::string cv = row[col_idx];
        std::string vv = row[val_idx];
        row_set.insert(rv);
        col_set.insert(cv);
        cells[rv][cv] = vv;
    }

    std::vector<std::string> row_headers(row_set.begin(), row_set.end());
    std::vector<std::string> col_headers(col_set.begin(), col_set.end());

    // Column widths: the first column is sized to the widest row header;
    // each subsequent column is sized to the widest of its header and cells.
    int row_w = 0;
    for (const auto& rh : row_headers) {
        row_w = std::max(row_w, static_cast<int>(rh.size()));
    }
    std::vector<int> col_widths(col_headers.size(), 0);
    for (size_t j = 0; j < col_headers.size(); ++j) {
        col_widths[j] = static_cast<int>(col_headers[j].size());
    }
    for (const auto& rh : row_headers) {
        auto it = cells.find(rh);
        for (size_t j = 0; j < col_headers.size(); ++j) {
            int cell_len = 0;
            if (it != cells.end()) {
                auto jt = it->second.find(col_headers[j]);
                if (jt != it->second.end()) {
                    cell_len = static_cast<int>(jt->second.size());
                }
            }
            col_widths[j] = std::max(col_widths[j], cell_len);
        }
    }

    // Header row: empty corner + column headers, separated by " | ".
    out << std::left << std::setw(row_w) << "";
    for (size_t j = 0; j < col_headers.size(); ++j) {
        out << " | " << std::left << std::setw(col_widths[j]) << col_headers[j];
    }
    out << "\n";

    // Data rows: row header + cells (empty string where no cell exists).
    for (const auto& rh : row_headers) {
        out << std::left << std::setw(row_w) << rh;
        auto it = cells.find(rh);
        for (size_t j = 0; j < col_headers.size(); ++j) {
            std::string cell;
            if (it != cells.end()) {
                auto jt = it->second.find(col_headers[j]);
                if (jt != it->second.end())
                    cell = jt->second;
            }
            out << " | " << std::left << std::setw(col_widths[j]) << cell;
        }
        out << "\n";
    }

    return CrosstabResult::kOk;
}

}  // namespace pgcpp::tools
