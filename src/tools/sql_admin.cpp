// sql_admin.cpp — SQL-driven admin tools (vacuumdb / reindexdb / clusterdb /
// createdb / dropdb / createuser / dropuser).
//
// Each tool builds a single SQL statement from its options struct, connects to
// the server via PsqlClient, and executes the statement. Identifier and literal
// quoting reuse the helpers from pg_dump.cpp.
#include "tools/sql_admin.hpp"

#include <string>

#include "tools/pg_dump.hpp"

namespace pgcpp::tools {

namespace {

// Connect to (host, port), execute `sql`, and report the outcome.
// `database` is the database the caller intends to operate on; PsqlClient does
// not expose a database selector, so it is currently unused.
AdminResult ExecuteAdminSql(const std::string& host, int port, const std::string& database,
                            const std::string& sql) {
    (void)database;  // PsqlClient has no database parameter.
    PsqlClient client(host, port);
    if (!client.Connect())
        return AdminResult::kConnectFailed;
    QueryResult r = client.ExecuteQuery(sql);
    client.Disconnect();
    if (!r.success)
        return AdminResult::kSqlFailed;
    return AdminResult::kOk;
}

}  // namespace

// --- Vacuum ---

std::string BuildVacuumSql(const VacuumOptions& opts) {
    std::string sql = "VACUUM";
    if (opts.full)
        sql += " FULL";
    if (opts.freeze)
        sql += " FREEZE";
    if (opts.verbose)
        sql += " VERBOSE";
    if (opts.analyze)
        sql += " ANALYZE";
    if (opts.skip_locked)
        sql += " SKIP_LOCKED";
    if (!opts.table.empty())
        sql += " " + QuoteIdentifier(opts.table);
    sql += ";";
    return sql;
}

AdminResult VacuumDatabase(const std::string& host, int port, const std::string& database,
                           const VacuumOptions& opts) {
    return ExecuteAdminSql(host, port, database, BuildVacuumSql(opts));
}

// --- Reindex ---

std::string BuildReindexSql(const ReindexOptions& opts) {
    const char* kind_word = nullptr;
    switch (opts.kind) {
        case ReindexOptions::Kind::kIndex:
            kind_word = "INDEX";
            break;
        case ReindexOptions::Kind::kTable:
            kind_word = "TABLE";
            break;
        case ReindexOptions::Kind::kSystem:
            kind_word = "SYSTEM";
            break;
        case ReindexOptions::Kind::kDatabase:
            kind_word = "DATABASE";
            break;
    }
    std::string sql = "REINDEX";
    if (opts.concurrently)
        sql += " CONCURRENTLY";
    sql += " ";
    sql += kind_word;
    if (opts.verbose)
        sql += " VERBOSE";
    if (!opts.name.empty())
        sql += " " + QuoteIdentifier(opts.name);
    sql += ";";
    return sql;
}

AdminResult ReindexDatabase(const std::string& host, int port, const std::string& database,
                            const ReindexOptions& opts) {
    return ExecuteAdminSql(host, port, database, BuildReindexSql(opts));
}

// --- Cluster ---

std::string BuildClusterSql(const ClusterOptions& opts) {
    std::string sql = "CLUSTER";
    if (opts.verbose)
        sql += " VERBOSE";
    if (!opts.table.empty())
        sql += " " + QuoteIdentifier(opts.table);
    if (!opts.index.empty())
        sql += " USING " + QuoteIdentifier(opts.index);
    sql += ";";
    return sql;
}

AdminResult ClusterDatabase(const std::string& host, int port, const std::string& database,
                            const ClusterOptions& opts) {
    return ExecuteAdminSql(host, port, database, BuildClusterSql(opts));
}

// --- Createdb / Dropdb ---

std::string BuildCreateDatabaseSql(const CreatedbOptions& opts) {
    std::string sql = "CREATE DATABASE " + QuoteIdentifier(opts.name);
    if (!opts.owner.empty())
        sql += " OWNER " + QuoteIdentifier(opts.owner);
    if (!opts.template_db.empty())
        sql += " TEMPLATE " + QuoteIdentifier(opts.template_db);
    if (!opts.encoding.empty())
        sql += " ENCODING " + QuoteLiteral(opts.encoding);
    if (!opts.lc_collate.empty())
        sql += " LC_COLLATE " + QuoteLiteral(opts.lc_collate);
    if (!opts.lc_ctype.empty())
        sql += " LC_CTYPE " + QuoteLiteral(opts.lc_ctype);
    sql += ";";
    return sql;
}

std::string BuildDropDatabaseSql(const std::string& name, bool if_exists) {
    std::string sql = "DROP DATABASE ";
    if (if_exists)
        sql += "IF EXISTS ";
    sql += QuoteIdentifier(name) + ";";
    return sql;
}

AdminResult CreateDatabase(const std::string& host, int port, const std::string& connect_db,
                           const CreatedbOptions& opts) {
    return ExecuteAdminSql(host, port, connect_db, BuildCreateDatabaseSql(opts));
}

AdminResult DropDatabase(const std::string& host, int port, const std::string& connect_db,
                         const std::string& name, bool if_exists) {
    return ExecuteAdminSql(host, port, connect_db, BuildDropDatabaseSql(name, if_exists));
}

// --- Createuser / Dropuser ---

std::string BuildCreateRoleSql(const CreateuserOptions& opts) {
    std::string sql = "CREATE ROLE " + QuoteIdentifier(opts.name) + " WITH";
    sql += opts.login ? " LOGIN" : " NOLOGIN";
    sql += opts.superuser ? " SUPERUSER" : " NOSUPERUSER";
    sql += opts.createdb ? " CREATEDB" : " NOCREATEDB";
    sql += opts.createrole ? " CREATEROLE" : " NOCREATEROLE";
    sql += opts.replication ? " REPLICATION" : " NOREPLICATION";
    if (!opts.password.empty())
        sql += " PASSWORD " + QuoteLiteral(opts.password);
    sql += ";";
    return sql;
}

std::string BuildDropRoleSql(const std::string& name, bool if_exists) {
    std::string sql = "DROP ROLE ";
    if (if_exists)
        sql += "IF EXISTS ";
    sql += QuoteIdentifier(name) + ";";
    return sql;
}

AdminResult CreateRole(const std::string& host, int port, const std::string& connect_db,
                       const CreateuserOptions& opts) {
    return ExecuteAdminSql(host, port, connect_db, BuildCreateRoleSql(opts));
}

AdminResult DropRole(const std::string& host, int port, const std::string& connect_db,
                     const std::string& name, bool if_exists) {
    return ExecuteAdminSql(host, port, connect_db, BuildDropRoleSql(name, if_exists));
}

}  // namespace pgcpp::tools
