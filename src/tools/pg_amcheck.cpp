// pg_amcheck.cpp — Relation consistency check (pg_amcheck).
//
// Issues amcheck SQL via libpq against heap relations and btree indexes,
// aggregates the per-relation outcomes into AmcheckStats, and returns a
// summary AmcheckResult. The amcheck extension must be installed in the
// target database; if CREATE EXTENSION fails, the run aborts with
// kAmcheckExtensionMissing.
#include "tools/pg_amcheck.hpp"

#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "libpq/libpq.hpp"

namespace pgcpp::tools {

namespace {

// QuoteLiteral — wrap in single quotes, doubling any embedded '.
std::string QuoteLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'')
            out.push_back('\'');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// RunSql — execute `sql` on `conn`, return the rows of the result.
// On error, returns false and sets *err_msg.
bool RunSql(pgcpp::libpq::PgConn& conn, const std::string& sql,
            std::vector<std::vector<std::string>>& rows, std::string* err_msg) {
    pgcpp::libpq::PgResult r = conn.Exec(sql);
    if (r.Status() == pgcpp::libpq::ExecStatusType::kFatalError) {
        if (err_msg)
            *err_msg = r.ErrorMessage();
        return false;
    }
    int n = r.NTuples();
    int nfields = r.NFields();
    rows.clear();
    rows.reserve(n);
    for (int i = 0; i < n; ++i) {
        std::vector<std::string> row;
        row.reserve(nfields);
        for (int j = 0; j < nfields; ++j) {
            const char* v = r.GetValue(i, j);
            row.emplace_back(v ? v : "");
        }
        rows.push_back(std::move(row));
    }
    return true;
}

// IssueOneCheck — run a check SQL and record outcome in `stats`.
// Returns true on success (no corruption detected, no error), false on
// corruption or I/O error.
bool IssueOneCheck(pgcpp::libpq::PgConn& conn, const std::string& sql, const std::string& label,
                   AmcheckStats& stats, std::ostream* verbose_out) {
    pgcpp::libpq::PgResult r = conn.Exec(sql);
    if (r.Status() == pgcpp::libpq::ExecStatusType::kFatalError) {
        ++stats.errors;
        if (verbose_out)
            *verbose_out << "ERROR  " << label << ": " << r.ErrorMessage() << "\n";
        return false;
    }
    // amcheck functions return true when corruption is found.
    if (r.NTuples() > 0 && r.NFields() > 0) {
        const char* v = r.GetValue(0, 0);
        std::string val = v ? v : "";
        if (InterpretCheckResult(val)) {
            ++stats.corrupt;
            if (verbose_out)
                *verbose_out << "CORRUPT " << label << "\n";
            return false;
        }
    }
    if (verbose_out)
        *verbose_out << "OK     " << label << "\n";
    return true;
}

// CheckOneDatabase — run all configured checks against a single database.
AmcheckResult CheckOneDatabase(const AmcheckOptions& opts, const std::string& dbname,
                               AmcheckStats& stats, std::ostream* verbose_out) {
    pgcpp::libpq::ConnectOptions co;
    co.host = opts.host;
    co.port = opts.port;
    co.dbname = dbname;
    pgcpp::libpq::PgConn conn;
    if (conn.Connect(co) != pgcpp::libpq::ConnStatusType::kOk)
        return AmcheckResult::kConnectFailed;

    // Ensure the amcheck extension is available.
    std::string err;
    std::vector<std::vector<std::string>> rows;
    if (!RunSql(conn, BuildCreateExtensionSql(), rows, &err)) {
        if (verbose_out)
            *verbose_out << "amcheck extension missing in " << dbname << ": " << err << "\n";
        conn.Finish();
        return AmcheckResult::kAmcheckExtensionMissing;
    }
    ++stats.databases_checked;

    // Heap checks.
    if (opts.heapall) {
        std::string sql = BuildAmcheckTableListSql(opts.table_pattern);
        if (!RunSql(conn, sql, rows, &err)) {
            conn.Finish();
            return AmcheckResult::kCatalogQueryFailed;
        }
        for (const auto& row : rows) {
            if (row.empty())
                continue;
            const std::string& tbl = row[0];
            ++stats.relations_checked;
            IssueOneCheck(conn, BuildHeapCheckSql(tbl), "heap " + tbl, stats, verbose_out);
        }
    }

    // Index checks.
    if (opts.index_check || opts.parent_check) {
        std::string sql = BuildListIndexesSql(opts.index_pattern);
        if (!RunSql(conn, sql, rows, &err)) {
            conn.Finish();
            return AmcheckResult::kCatalogQueryFailed;
        }
        for (const auto& row : rows) {
            if (row.empty())
                continue;
            const std::string& idx = row[0];
            ++stats.indexes_checked;
            if (opts.index_check)
                IssueOneCheck(conn, BuildIndexCheckSql(idx), "index " + idx, stats, verbose_out);
            if (opts.parent_check)
                IssueOneCheck(conn, BuildIndexParentCheckSql(idx), "pindex " + idx, stats,
                              verbose_out);
        }
    }

    conn.Finish();
    return AmcheckResult::kOk;
}

}  // namespace

std::string BuildHeapCheckSql(const std::string& table_name) {
    std::ostringstream oss;
    oss << "SELECT * FROM verify_heapam(" << QuoteLiteral(table_name) << "::regclass);";
    return oss.str();
}

std::string BuildIndexCheckSql(const std::string& index_name) {
    std::ostringstream oss;
    oss << "SELECT bt_index_check(" << QuoteLiteral(index_name) << "::regclass);";
    return oss.str();
}

std::string BuildIndexParentCheckSql(const std::string& index_name) {
    std::ostringstream oss;
    oss << "SELECT bt_index_parent_check(" << QuoteLiteral(index_name) << "::regclass);";
    return oss.str();
}

std::string BuildAmcheckTableListSql(const std::string& table_pattern) {
    std::ostringstream oss;
    oss << "SELECT c.relname FROM pg_class c "
        << "JOIN pg_namespace n ON n.oid = c.relnamespace "
        << "WHERE c.relkind = 'r' AND n.nspname = 'public'";
    if (!table_pattern.empty())
        oss << " AND c.relname LIKE " << QuoteLiteral("%" + table_pattern + "%");
    oss << " ORDER BY c.relname;";
    return oss.str();
}

std::string BuildListIndexesSql(const std::string& index_pattern) {
    std::ostringstream oss;
    oss << "SELECT c.relname FROM pg_class c "
        << "JOIN pg_namespace n ON n.oid = c.relnamespace "
        << "WHERE c.relkind = 'i' AND n.nspname = 'public'";
    if (!index_pattern.empty())
        oss << " AND c.relname LIKE " << QuoteLiteral("%" + index_pattern + "%");
    oss << " ORDER BY c.relname;";
    return oss.str();
}

std::string BuildAmcheckDatabaseListSql() {
    return std::string(
        "SELECT datname FROM pg_database "
        "WHERE datallowconn AND NOT datistemplate ORDER BY 1;");
}

std::string BuildCreateExtensionSql() {
    return std::string("CREATE EXTENSION IF NOT EXISTS amcheck;");
}

bool InterpretCheckResult(const std::string& value) {
    // amcheck functions return true / 't' when corruption is found.
    return value == "t" || value == "true" || value == "T" || value == "TRUE";
}

AmcheckResult RunAmcheck(const AmcheckOptions& opts, AmcheckStats& stats,
                         std::ostream* verbose_out) {
    // Build the list of databases to check.
    std::vector<std::string> dbnames;
    if (opts.all_db) {
        pgcpp::libpq::ConnectOptions co;
        co.host = opts.host;
        co.port = opts.port;
        co.dbname = "postgres";
        pgcpp::libpq::PgConn conn;
        if (conn.Connect(co) != pgcpp::libpq::ConnStatusType::kOk)
            return AmcheckResult::kConnectFailed;
        std::vector<std::vector<std::string>> rows;
        std::string err;
        if (!RunSql(conn, BuildAmcheckDatabaseListSql(), rows, &err)) {
            conn.Finish();
            return AmcheckResult::kCatalogQueryFailed;
        }
        for (const auto& row : rows) {
            if (!row.empty())
                dbnames.push_back(row[0]);
        }
        conn.Finish();
    } else {
        dbnames.push_back(opts.dbname);
    }

    if (dbnames.empty())
        return AmcheckResult::kNoRelationsFound;

    bool found_any = false;
    bool any_corrupt = false;
    bool any_error = false;
    bool any_connect_failed = false;
    bool all_connect_failed = true;
    for (const auto& db : dbnames) {
        AmcheckStats local;
        AmcheckResult r = CheckOneDatabase(opts, db, local, verbose_out);
        stats.databases_checked += local.databases_checked;
        stats.relations_checked += local.relations_checked;
        stats.indexes_checked += local.indexes_checked;
        stats.corrupt += local.corrupt;
        stats.errors += local.errors;
        if (r == AmcheckResult::kConnectFailed) {
            any_connect_failed = true;
            any_error = true;
            continue;
        }
        if (r == AmcheckResult::kCatalogQueryFailed) {
            any_error = true;
            all_connect_failed = false;
            continue;
        }
        if (r == AmcheckResult::kAmcheckExtensionMissing) {
            any_error = true;
            all_connect_failed = false;
            continue;
        }
        all_connect_failed = false;
        if (local.relations_checked > 0 || local.indexes_checked > 0)
            found_any = true;
        if (local.corrupt > 0)
            any_corrupt = true;
        if (local.errors > 0)
            any_error = true;
    }

    // If every database we tried to reach failed at the connect step,
    // report that explicitly.
    if (all_connect_failed && any_connect_failed)
        return AmcheckResult::kConnectFailed;
    if (!found_any && !any_error)
        return AmcheckResult::kNoRelationsFound;
    if (any_corrupt)
        return AmcheckResult::kCorruptionFound;
    if (any_error)
        return AmcheckResult::kCatalogQueryFailed;
    return AmcheckResult::kOk;
}

}  // namespace pgcpp::tools
