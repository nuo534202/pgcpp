// psql.cpp — MyToyDB command-line client (psql equivalent).
//
// Converted from PostgreSQL 15's src/bin/psql/.
//
// Connects to a MyToyDB server and executes SQL queries interactively
// or from a file/command-line argument.
//
// Usage:
//   psql [-h host] [-p port] [-c "SQL"] [-f file.sql]
//
// If neither -c nor -f is given, reads SQL from stdin interactively.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "mytoydb/tools/psql_client.h"

using mytoydb::tools::FormatQueryResult;
using mytoydb::tools::PsqlClient;
using mytoydb::tools::QueryResult;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "MyToyDB client (psql equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options]\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>    Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>    Server port (default: 5433)\n");
    std::fprintf(stderr, "  -c <sql>     Execute a single SQL command and exit\n");
    std::fprintf(stderr, "  -f <file>    Execute SQL from file and exit\n");
    std::fprintf(stderr, "  --help       Show this help\n");
}

struct PsqlOptions {
    std::string host = "127.0.0.1";
    int port = 5433;
    std::string command;
    std::string file;
    bool show_help = false;
};

bool ParseArgs(int argc, char* argv[], PsqlOptions* opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            opts->show_help = true;
            return true;
        }
        if (arg == "-h") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -h requires an argument\n");
                return false;
            }
            opts->host = argv[++i];
        } else if (arg == "-p") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -p requires an argument\n");
                return false;
            }
            opts->port = std::atoi(argv[++i]);
        } else if (arg == "-c") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -c requires an argument\n");
                return false;
            }
            opts->command = argv[++i];
        } else if (arg == "-f") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -f requires an argument\n");
                return false;
            }
            opts->file = argv[++i];
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            return false;
        }
    }
    return true;
}

// Execute a query and print the result.
void ExecuteAndPrint(PsqlClient& client, const std::string& query) {
    QueryResult result = client.ExecuteQuery(query);
    std::cout << FormatQueryResult(result);
}

// Read and execute SQL from a file.
// Statements are separated by semicolons (simple splitting — no full SQL parsing).
void ExecuteFile(PsqlClient& client, const std::string& filename) {
    std::ifstream f(filename);
    if (!f.is_open()) {
        std::fprintf(stderr, "error: cannot open file '%s'\n", filename.c_str());
        return;
    }

    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    // Split by semicolons (naive but sufficient for ClickBench queries).
    std::string stmt;
    bool in_string = false;
    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (c == '\'') {
            in_string = !in_string;
        }
        if (c == ';' && !in_string) {
            // Trim whitespace.
            size_t start = stmt.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) {
                stmt = stmt.substr(start);
                ExecuteAndPrint(client, stmt);
            }
            stmt.clear();
        } else {
            stmt.push_back(c);
        }
    }
    // Execute any remaining statement.
    size_t start = stmt.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
        stmt = stmt.substr(start);
        ExecuteAndPrint(client, stmt);
    }
}

// Interactive mode: read SQL from stdin, execute on Enter.
void InteractiveMode(PsqlClient& client) {
    std::cout << "MyToyDB client — type \\q to quit, \\? for help\n";

    std::string buffer;
    bool in_string = false;

    while (true) {
        // Print prompt.
        if (buffer.empty()) {
            std::cout << "mytoydb=> " << std::flush;
        } else {
            std::cout << "mytoydb-> " << std::flush;
        }

        std::string line;
        if (!std::getline(std::cin, line)) {
            break;  // EOF
        }

        // Check for backslash commands.
        if (buffer.empty() && !line.empty() && line[0] == '\\') {
            if (line == "\\q" || line == "\\quit") {
                break;
            }
            if (line == "\\?" || line == "\\help") {
                std::cout << "  \\q    quit\n  \\?    show this help\n";
                continue;
            }
            std::cout << "unknown command: " << line << "\n";
            continue;
        }

        // Append to buffer.
        buffer += line;
        buffer += "\n";

        // Check if the statement is complete (ends with semicolon outside string).
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\'') {
                in_string = !in_string;
            }
        }

        if (!in_string && !buffer.empty()) {
            // Check if the last non-whitespace character is a semicolon.
            size_t end = buffer.find_last_not_of(" \t\n\r");
            if (end != std::string::npos && buffer[end] == ';') {
                // Execute the statement.
                std::string stmt = buffer.substr(0, end + 1);
                ExecuteAndPrint(client, stmt);
                buffer.clear();
                in_string = false;
            }
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    PsqlOptions opts;
    if (!ParseArgs(argc, argv, &opts)) {
        PrintUsage(argv[0]);
        return 1;
    }

    if (opts.show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    // Connect to the server.
    PsqlClient client(opts.host, opts.port);
    if (!client.Connect()) {
        std::fprintf(stderr, "error: could not connect to server at %s:%d\n",
                     opts.host.c_str(), opts.port);
        return 1;
    }

    if (!opts.command.empty()) {
        // Execute a single command.
        ExecuteAndPrint(client, opts.command);
    } else if (!opts.file.empty()) {
        // Execute from file.
        ExecuteFile(client, opts.file);
    } else {
        // Interactive mode.
        InteractiveMode(client);
    }

    client.Disconnect();
    return 0;
}
