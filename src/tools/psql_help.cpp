// psql_help.cpp — SQL command help (help.c).
//
// Converted from PostgreSQL 15's src/bin/psql/help.c and the generated
// sql_help.h (from sql_help.txt).
//
// Provides a hand-curated help table for the common SQL statements exposed
// via psql's `\h <topic>` meta-command.
#include "tools/psql_help.hpp"

#include <cctype>
#include <ostream>
#include <string>
#include <vector>

namespace pgcpp::tools {

namespace {

// ToUpper — return an uppercased copy of `s` (ASCII only). Used for the
// case-insensitive topic lookup, since all stored topic names are uppercase.
std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

const std::vector<SqlHelpTopic>& GetSqlHelpTopics() {
    // A static local guarantees construction on first use and thread-safe
    // initialisation (C++11 guarantees this). The returned reference is valid
    // for the lifetime of the program.
    static const std::vector<SqlHelpTopic> kTopics = {
        {"SELECT", "SELECT select_list FROM table [WHERE condition] [ORDER BY ...]",
         "retrieve rows from a table or view"},
        {"INSERT", "INSERT INTO table [(columns)] {VALUES (values) | query}",
         "create new rows in a table"},
        {"UPDATE", "UPDATE table SET column = expr [, ...] [WHERE condition]",
         "update rows of a table"},
        {"DELETE", "DELETE FROM table [WHERE condition]", "delete rows of a table"},
        {"CREATE TABLE", "CREATE TABLE name (column type [, ...])", "define a new table"},
        {"DROP TABLE", "DROP TABLE [IF EXISTS] name [, ...] [CASCADE|RESTRICT]", "remove a table"},
        {"CREATE INDEX", "CREATE [UNIQUE] INDEX name ON table (column [, ...])",
         "define a new index"},
        {"DROP INDEX", "DROP INDEX [IF EXISTS] name [, ...] [CASCADE|RESTRICT]", "remove an index"},
        {"CREATE VIEW", "CREATE VIEW name AS query", "define a new view"},
        {"DROP VIEW", "DROP VIEW [IF EXISTS] name [, ...] [CASCADE|RESTRICT]", "remove a view"},
        {"BEGIN", "BEGIN [TRANSACTION | WORK]", "start a transaction block"},
        {"COMMIT", "COMMIT [TRANSACTION | WORK]", "commit the current transaction"},
        {"ROLLBACK", "ROLLBACK [TRANSACTION | WORK]", "abort the current transaction"},
        {"VACUUM", "VACUUM [FULL | ANALYZE] [table]", "garbage-collect and analyze a database"},
        {"REINDEX", "REINDEX [INDEX|TABLE|DATABASE] name", "rebuild indexes"},
        {"CLUSTER", "CLUSTER [VERBOSE] table [USING index]",
         "cluster a table according to an index"},
        {"COPY", "COPY table FROM 'file' | COPY query TO 'file'",
         "copy data between a file and a table"},
        {"EXPLAIN", "EXPLAIN [ANALYZE] statement", "show the execution plan of a statement"},
        {"TRUNCATE", "TRUNCATE [TABLE] name [, ...] [CASCADE|RESTRICT]",
         "empty a table or set of tables"},
        {"GRANT", "GRANT {privs | ALL} ON object TO role", "define access privileges"},
        {"REVOKE", "REVOKE {privs | ALL} ON object FROM role", "remove access privileges"},
    };
    return kTopics;
}

const SqlHelpTopic* FindSqlHelpTopic(const std::string& name) {
    std::string upper = ToUpper(name);
    for (const auto& topic : GetSqlHelpTopics()) {
        if (topic.name == upper) {
            return &topic;
        }
    }
    return nullptr;
}

bool PrintSqlHelp(const std::string& topic, std::ostream& out) {
    const SqlHelpTopic* t = FindSqlHelpTopic(topic);
    if (t == nullptr) {
        return false;
    }
    out << "Command: " << t->name << "\n";
    out << "Description: " << t->description << "\n";
    out << "Syntax:\n" << t->syntax << "\n";
    return true;
}

void PrintSqlHelpIndex(std::ostream& out) {
    out << "Available help:\n";
    for (const auto& topic : GetSqlHelpTopics()) {
        out << topic.name << "\n";
    }
}

void PrintPsqlHelp(std::ostream& out) {
    out << "psql is the PostgreSQL interactive terminal.\n\n";
    out << "General backslash commands:\n";
    out << "  \\?              show this help\n";
    out << "  \\q              quit psql\n";
    out << "  \\h [NAME]       help on SQL command syntax\n";
    out << "  \\d [NAME]       describe relation (table, view, etc.)\n";
    out << "  \\dt [PATTERN]   list tables\n";
    out << "  \\l              list databases\n";
    out << "  \\du             list roles\n";
    out << "  \\echo TEXT      echo text\n";
    out << "  \\i FILE         execute commands from file\n";
    out << "  \\set [VAR VAL]  set or list psql variables\n";
    out << "  \\unset VAR      unset psql variable\n";
    out << "  \\pset [KEY VAL] set table output option\n";
}

}  // namespace pgcpp::tools
