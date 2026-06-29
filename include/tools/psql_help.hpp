// psql_help.h — SQL command help (help.c).
//
// Converted from PostgreSQL 15's src/bin/psql/help.c and the generated
// sql_help.h (from sql_help.txt).
//
// PG ships a built-in `\h <topic>` command that prints syntax help for SQL
// statements (e.g. `\h SELECT` prints the SELECT grammar). The data is
// generated from the grammar at build time.
//
// pgcpp provides a hand-curated subset covering the common SQL statements:
//   SELECT, INSERT, UPDATE, DELETE, CREATE TABLE, DROP TABLE, CREATE INDEX,
//   DROP INDEX, CREATE VIEW, DROP VIEW, BEGIN, COMMIT, ROLLBACK, VACUUM,
//   REINDEX, CLUSTER, COPY, EXPLAIN, TRUNCATE, GRANT, REVOKE.
#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace pgcpp::tools {

// SqlHelpTopic — one entry in the help table.
struct SqlHelpTopic {
    std::string name;         // uppercase keyword, e.g. "SELECT"
    std::string syntax;       // the grammar (one line, abstract)
    std::string description;  // short description
};

// GetSqlHelpTopics — return all known help topics.
const std::vector<SqlHelpTopic>& GetSqlHelpTopics();

// FindSqlHelpTopic — return the topic with the given name (case-insensitive),
// or nullptr if not found.
const SqlHelpTopic* FindSqlHelpTopic(const std::string& name);

// PrintSqlHelp — print help for a single topic to `out`.
// Returns true if the topic was found.
bool PrintSqlHelp(const std::string& topic, std::ostream& out);

// PrintSqlHelpIndex — print the list of all known topics to `out`.
void PrintSqlHelpIndex(std::ostream& out);

// PrintPsqlHelp — print the general psql help (the `\?` output) to `out`.
void PrintPsqlHelp(std::ostream& out);

}  // namespace pgcpp::tools
