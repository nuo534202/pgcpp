// psql_command.cpp — Backslash meta-command dispatcher for psql.
//
// Converted from PostgreSQL 15's src/bin/psql/command.c::exec_command.
//
// The dispatcher tokenizes a backslash command line, looks up the command
// name, and either:
//   - prints help/error text directly to `out` (\?, \echo, unknown, etc.),
//   - mutates the psql variable map (\set, \unset), or
//   - sends a SQL query through PsqlClient and prints the formatted result
//     (\dt, \dv, \l, \du, \d).
//
// The function returns kQuit only for \q/\quit so the interactive loop can
// exit cleanly. For all other commands it returns kContinue.
#include "mytoydb/tools/psql_command.hpp"

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mytoydb/tools/psql_client.hpp"
#include "mytoydb/tools/psql_describe.hpp"

namespace mytoydb::tools {

namespace {

// Tokenize a backslash command line.
//
// The leading backslash introduces the command name (e.g. "dt" in
// "\\dt foo"). Subsequent tokens are separated by whitespace. Single- or
// double-quoted tokens preserve embedded whitespace. Backslash escapes
// are NOT supported (matching PostgreSQL's psql simple tokenizer).
//
// Returns the tokens with the command name at index 0 (without the
// backslash). Returns an empty vector if no command name was found.
std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    size_t i = 0;

    // Skip leading whitespace.
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        ++i;
    if (i >= line.size() || line[i] != '\\') {
        return tokens;
    }
    ++i;  // consume backslash

    // Read the command name (alphanumeric, '?', or '?' is special — \? is
    // the help command and has no trailing name).
    std::string cmd;
    while (i < line.size()) {
        char c = line[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '?' || c == '_') {
            cmd.push_back(c);
            ++i;
        } else {
            break;
        }
    }
    if (cmd.empty()) {
        return tokens;
    }
    tokens.push_back(cmd);

    // Read remaining arguments.
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i >= line.size())
            break;

        char c = line[i];
        std::string tok;
        if (c == '"' || c == '\'') {
            // Quoted argument: read until matching quote.
            char quote = c;
            ++i;
            while (i < line.size() && line[i] != quote) {
                tok.push_back(line[i++]);
            }
            if (i < line.size())
                ++i;  // consume closing quote
        } else {
            // Unquoted argument: read until next whitespace.
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
                tok.push_back(line[i++]);
            }
        }
        tokens.push_back(tok);
    }
    return tokens;
}

// Substitute :var references in `text` using `vars`. Only a single leading
// :var token (the entire text) is substituted — this matches psql's
// \echo behavior for simple variable references.
std::string SubstituteVar(const std::string& text, const std::map<std::string, std::string>& vars) {
    if (text.size() > 1 && text[0] == ':') {
        auto it = vars.find(text.substr(1));
        if (it != vars.end()) {
            return it->second;
        }
    }
    return text;
}

void PrintHelp(std::ostream& out) {
    out << "Available meta-commands:\n";
    out << "  \\q             quit\n";
    out << "  \\?             show this help\n";
    out << "  \\d [NAME]      describe relation (or list tables)\n";
    out << "  \\dt [PATTERN]  list tables\n";
    out << "  \\dv [PATTERN]  list views\n";
    out << "  \\l             list databases\n";
    out << "  \\du            list roles\n";
    out << "  \\c DBNAME      connect to database (stub)\n";
    out << "  \\echo TEXT     echo text (supports :var substitution)\n";
    out << "  \\i FILE        execute SQL from file\n";
    out << "  \\set [VAR VAL] set/list psql variables\n";
    out << "  \\unset VAR     unset psql variable\n";
}

}  // namespace

MetaCommandResult ExecuteMetaCommand(PsqlClient& client, const std::string& line,
                                     std::map<std::string, std::string>& vars, std::ostream& out) {
    std::vector<std::string> tokens = Tokenize(line);
    if (tokens.empty()) {
        out << "Invalid command: " << line << "\n";
        return MetaCommandResult::kContinue;
    }

    const std::string& cmd = tokens[0];

    // --- Control flow commands -------------------------------------------
    if (cmd == "q" || cmd == "quit") {
        return MetaCommandResult::kQuit;
    }
    if (cmd == "?" || cmd == "help") {
        PrintHelp(out);
        return MetaCommandResult::kContinue;
    }

    // --- Local state commands (no server contact) ------------------------
    if (cmd == "echo") {
        std::string text;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (i > 1)
                text += " ";
            text += tokens[i];
        }
        text = SubstituteVar(text, vars);
        out << text << "\n";
        return MetaCommandResult::kContinue;
    }

    if (cmd == "set") {
        if (tokens.size() < 2) {
            // \set with no arguments — list all variables.
            for (const auto& [k, v] : vars) {
                out << k << " = " << v << "\n";
            }
        } else {
            std::string var = tokens[1];
            std::string val;
            for (size_t i = 2; i < tokens.size(); ++i) {
                if (i > 2)
                    val += " ";
                val += tokens[i];
            }
            vars[var] = val;
        }
        return MetaCommandResult::kContinue;
    }

    if (cmd == "unset") {
        if (tokens.size() < 2) {
            out << "\\unset: missing required argument (variable name)\n";
            return MetaCommandResult::kContinue;
        }
        vars.erase(tokens[1]);
        return MetaCommandResult::kContinue;
    }

    // --- File commands ---------------------------------------------------
    if (cmd == "i" || cmd == "include") {
        if (tokens.size() < 2) {
            out << "\\i: missing required argument (filename)\n";
            return MetaCommandResult::kContinue;
        }
        const std::string& filename = tokens[1];
        std::ifstream f(filename);
        if (!f.is_open()) {
            out << "could not open file: " << filename << "\n";
            return MetaCommandResult::kContinue;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        // Send the entire file as one query string. The simple query
        // protocol supports multiple statements separated by ';'.
        QueryResult r = client.ExecuteQuery(ss.str());
        out << FormatQueryResult(r);
        return MetaCommandResult::kContinue;
    }

    // --- Connection management ------------------------------------------
    if (cmd == "c" || cmd == "connect") {
        if (tokens.size() < 2) {
            out << "\\c: missing required argument (database name)\n";
            return MetaCommandResult::kContinue;
        }
        // MyToyDB doesn't currently support reconnecting on the same TCP
        // socket — \c is a stub that acknowledges the request.
        out << "You are now connected to database \"" << tokens[1]
            << "\" (stub: live reconnect not supported).\n";
        return MetaCommandResult::kContinue;
    }

    // --- Catalog query commands -----------------------------------------
    if (cmd == "dt") {
        std::string pattern = tokens.size() > 1 ? tokens[1] : "";
        QueryResult r = client.ExecuteQuery(BuildListTablesSql(pattern));
        out << FormatQueryResult(r);
        return MetaCommandResult::kContinue;
    }
    if (cmd == "dv") {
        std::string pattern = tokens.size() > 1 ? tokens[1] : "";
        QueryResult r = client.ExecuteQuery(BuildListViewsSql(pattern));
        out << FormatQueryResult(r);
        return MetaCommandResult::kContinue;
    }
    if (cmd == "l") {
        QueryResult r = client.ExecuteQuery(BuildListDatabasesSql());
        out << FormatQueryResult(r);
        return MetaCommandResult::kContinue;
    }
    if (cmd == "du") {
        QueryResult r = client.ExecuteQuery(BuildListRolesSql());
        out << FormatQueryResult(r);
        return MetaCommandResult::kContinue;
    }
    if (cmd == "d") {
        if (tokens.size() < 2) {
            // Bare \d lists user tables (like \dt).
            QueryResult r = client.ExecuteQuery(BuildListTablesSql(""));
            out << FormatQueryResult(r);
        } else {
            QueryResult r = client.ExecuteQuery(BuildDescribeRelationSql(tokens[1]));
            out << FormatQueryResult(r);
        }
        return MetaCommandResult::kContinue;
    }

    out << "unknown command: " << line << "\n";
    return MetaCommandResult::kContinue;
}

}  // namespace mytoydb::tools
