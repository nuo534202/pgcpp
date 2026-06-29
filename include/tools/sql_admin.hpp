// sql_admin.h — SQL-driven admin tools (vacuumdb/reindexdb/clusterdb/
//                              createdb/dropdb/createuser/dropuser).
//
// Converted from PostgreSQL 15's src/bin/{vacuumdb,reindexdb,clusterdb,
// createdb,dropdb,createuser,dropuser}/.
//
// These seven tools are thin wrappers around SQL commands sent via PsqlClient:
//   vacuumdb    -> VACUUM [FULL] [ANALYZE] [table]
//   reindexdb   -> REINDEX [INDEX|TABLE|DATABASE|SYSTEM] [CONCURRENTLY] [name]
//   clusterdb   -> CLUSTER [VERBOSE] [table [USING index]]
//   createdb    -> CREATE DATABASE name [OWNER owner] [TEMPLATE tmpl]
//   dropdb      -> DROP DATABASE [IF EXISTS] name
//   createuser  -> CREATE ROLE name [LOGIN] [SUPERUSER] [PASSWORD 'pw']
//   dropuser    -> DROP ROLE [IF EXISTS] name
//
// pgcpp consolidates them into one module because their logic is identical
// (parse args, connect, execute one SQL statement, report). Each tool has
// its own thin CLI wrapper.
#pragma once

#include <string>

#include "tools/psql_client.hpp"

namespace pgcpp::tools {

// VacuumOptions — inputs to vacuumdb.
struct VacuumOptions {
    bool full = false;
    bool analyze = false;
    bool verbose = false;
    bool freeze = false;
    bool skip_locked = false;
    // Empty = all tables.
    std::string table;
};

// ReindexOptions — inputs to reindexdb.
struct ReindexOptions {
    enum class Kind { kIndex, kTable, kDatabase, kSystem };
    Kind kind = Kind::kDatabase;
    bool concurrently = false;
    bool verbose = false;
    std::string name;  // index/table/database name
};

// ClusterOptions — inputs to clusterdb.
struct ClusterOptions {
    bool verbose = false;
    std::string table;
    std::string index;
};

// CreatedbOptions — inputs to createdb.
struct CreatedbOptions {
    std::string name;
    std::string owner;
    std::string template_db;
    std::string encoding;
    std::string lc_collate;
    std::string lc_ctype;
};

// CreateuserOptions — inputs to createuser.
struct CreateuserOptions {
    std::string name;
    bool superuser = false;
    bool login = true;  // default CREATEROLE
    bool createdb = false;
    bool createrole = false;
    bool replication = false;
    std::string password;
};

// --- Result type ---

enum class AdminResult {
    kOk,
    kConnectFailed,
    kSqlFailed,
};

// --- Vacuum ---

// BuildVacuumSql — build the SQL statement for the given VacuumOptions.
std::string BuildVacuumSql(const VacuumOptions& opts);

// VacuumDatabase — connect and run VACUUM.
AdminResult VacuumDatabase(const std::string& host, int port, const std::string& database,
                           const VacuumOptions& opts);

// --- Reindex ---

// BuildReindexSql — build the SQL statement for the given ReindexOptions.
std::string BuildReindexSql(const ReindexOptions& opts);

// ReindexDatabase — connect and run REINDEX.
AdminResult ReindexDatabase(const std::string& host, int port, const std::string& database,
                            const ReindexOptions& opts);

// --- Cluster ---

// BuildClusterSql — build the SQL statement for the given ClusterOptions.
std::string BuildClusterSql(const ClusterOptions& opts);

// ClusterDatabase — connect and run CLUSTER.
AdminResult ClusterDatabase(const std::string& host, int port, const std::string& database,
                            const ClusterOptions& opts);

// --- Createdb / Dropdb ---

// BuildCreateDatabaseSql — build the SQL statement for createdb.
std::string BuildCreateDatabaseSql(const CreatedbOptions& opts);

// BuildDropDatabaseSql — build the SQL statement for dropdb.
std::string BuildDropDatabaseSql(const std::string& name, bool if_exists);

// CreateDatabase — connect and run CREATE DATABASE.
AdminResult CreateDatabase(const std::string& host, int port, const std::string& connect_db,
                           const CreatedbOptions& opts);

// DropDatabase — connect and run DROP DATABASE.
AdminResult DropDatabase(const std::string& host, int port, const std::string& connect_db,
                         const std::string& name, bool if_exists);

// --- Createuser / Dropuser ---

// BuildCreateRoleSql — build the SQL statement for createuser.
std::string BuildCreateRoleSql(const CreateuserOptions& opts);

// BuildDropRoleSql — build the SQL statement for dropuser.
std::string BuildDropRoleSql(const std::string& name, bool if_exists);

// CreateRole — connect and run CREATE ROLE.
AdminResult CreateRole(const std::string& host, int port, const std::string& connect_db,
                       const CreateuserOptions& opts);

// DropRole — connect and run DROP ROLE.
AdminResult DropRole(const std::string& host, int port, const std::string& connect_db,
                     const std::string& name, bool if_exists);

}  // namespace pgcpp::tools
