// pg_dump.cpp — Database dump utility (pg_dump).
//
// Implements the simplified text-format dump: connect to a server, query the
// system catalog for tables and columns, and emit DROP/CREATE/COPY/INSERT
// statements that can be replayed by psql or pg_restore.
#include "pgcpp/tools/pg_dump.hpp"

#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pgcpp::tools {

namespace {

// Check whether `name` matches `pattern` (substring match; empty pattern = all).
bool TableNameMatches(const std::string& name, const std::string& pattern) {
    if (pattern.empty())
        return true;
    return name.find(pattern) != std::string::npos;
}

}  // namespace

std::string QuoteIdentifier(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"')
            out.push_back('"');  // double embedded double quotes
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string QuoteLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'')
            out.push_back('\'');  // double embedded single quotes
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string BuildDropTableStatement(const std::string& table_name) {
    return "DROP TABLE IF EXISTS " + QuoteIdentifier(table_name) + ";\n";
}

std::string BuildCreateTableStatement(
    const std::string& table_name,
    const std::vector<std::pair<std::string, std::string>>& columns) {
    std::ostringstream oss;
    oss << "CREATE TABLE " << QuoteIdentifier(table_name) << " (\n";
    for (size_t i = 0; i < columns.size(); ++i) {
        oss << "  " << QuoteIdentifier(columns[i].first) << " " << columns[i].second;
        if (i + 1 < columns.size())
            oss << ",";
        oss << "\n";
    }
    oss << ");\n";
    return oss.str();
}

std::string BuildCopyHeader(const std::string& table_name,
                            const std::vector<std::string>& column_names) {
    std::ostringstream oss;
    oss << "COPY " << QuoteIdentifier(table_name) << " (";
    for (size_t i = 0; i < column_names.size(); ++i) {
        if (i > 0)
            oss << ", ";
        oss << QuoteIdentifier(column_names[i]);
    }
    oss << ") FROM stdin;\n";
    return oss.str();
}

std::string BuildInsertStatement(const std::string& table_name,
                                 const std::vector<std::string>& column_names,
                                 const std::vector<std::string>& values) {
    std::ostringstream oss;
    oss << "INSERT INTO " << QuoteIdentifier(table_name) << " (";
    for (size_t i = 0; i < column_names.size(); ++i) {
        if (i > 0)
            oss << ", ";
        oss << QuoteIdentifier(column_names[i]);
    }
    oss << ") VALUES (";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0)
            oss << ", ";
        oss << QuoteLiteral(values[i]);
    }
    oss << ");\n";
    return oss.str();
}

DumpResult DumpDatabase(const std::string& host, int port, const DumpOptions& opts,
                        std::ostream& out) {
    PsqlClient client(host, port);
    if (!client.Connect())
        return DumpResult::kConnectFailed;

    // Header comment.
    out << "-- pgcpp database dump\n"
        << "-- database: " << opts.database << "\n\n";

    std::vector<std::string> tables;

    // Step 3: schema emission (skipped when --data-only).
    if (!opts.data_only) {
        QueryResult r = client.ExecuteQuery(
            "SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1");
        if (!r.success) {
            client.Disconnect();
            return DumpResult::kCatalogQueryFailed;
        }
        for (const auto& row : r.rows) {
            if (row.empty())
                continue;
            if (!TableNameMatches(row[0], opts.table_pattern))
                continue;
            tables.push_back(row[0]);
        }

        for (const auto& table : tables) {
            if (opts.clean)
                out << BuildDropTableStatement(table);

            std::string cols_query =
                "SELECT column_name, data_type FROM information_schema.columns "
                "WHERE table_name = " +
                QuoteLiteral(table) + " ORDER BY ordinal_position";
            QueryResult cr = client.ExecuteQuery(cols_query);
            if (!cr.success) {
                client.Disconnect();
                return DumpResult::kCatalogQueryFailed;
            }
            std::vector<std::pair<std::string, std::string>> columns;
            for (const auto& row : cr.rows) {
                if (row.size() >= 2)
                    columns.emplace_back(row[0], row[1]);
            }
            out << BuildCreateTableStatement(table, columns);
        }
    }

    // Step 4: data emission (skipped when --schema-only).
    if (!opts.schema_only) {
        for (const auto& table : tables) {
            std::string data_query = "SELECT * FROM " + QuoteIdentifier(table);
            QueryResult dr = client.ExecuteQuery(data_query);
            if (!dr.success) {
                client.Disconnect();
                return DumpResult::kCatalogQueryFailed;
            }

            if (opts.inserts) {
                for (const auto& row : dr.rows)
                    out << BuildInsertStatement(table, dr.column_names, row);
            } else {
                out << BuildCopyHeader(table, dr.column_names);
                for (const auto& row : dr.rows) {
                    for (size_t i = 0; i < row.size(); ++i) {
                        if (i > 0)
                            out << '\t';
                        out << row[i];
                    }
                    out << '\n';
                }
                out << "\\.\n";
            }
        }
    }

    client.Disconnect();

    // Step 5: report no tables found (only when not --data-only).
    if (!opts.data_only && tables.empty())
        return DumpResult::kNoTablesFound;
    return DumpResult::kOk;
}

}  // namespace pgcpp::tools
