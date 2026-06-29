// dropdb.cpp — pgcpp DROP DATABASE utility (dropdb equivalent).
//
// Converted from PostgreSQL 15's src/bin/dropdb/.
//
// Connects to a running server and issues a DROP DATABASE statement.
//
// Usage:
//   dropdb [-h host] [-p port] [-d connect_db] [--if-exists] <name>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "tools/sql_admin.hpp"

using pgcpp::tools::AdminResult;
using pgcpp::tools::DropDatabase;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp drop-database utility (dropdb equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options] <dbname>\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>          Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>          Server port (default: 5433)\n");
    std::fprintf(stderr, "  -d <database>      Database to connect to (default: pgcpp)\n");
    std::fprintf(stderr, "  --if-exists        Use IF EXISTS\n");
    std::fprintf(stderr, "  --help             Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 5433;
    std::string connect_db = "pgcpp";
    std::string name;
    bool if_exists = false;
    bool show_help = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            show_help = true;
        } else if (arg == "-h") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -h requires an argument\n");
                return 1;
            }
            host = argv[++i];
        } else if (arg == "-p") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -p requires an argument\n");
                return 1;
            }
            port = std::atoi(argv[++i]);
        } else if (arg == "-d" || arg == "--dbname") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -d requires an argument\n");
                return 1;
            }
            connect_db = argv[++i];
        } else if (arg == "--if-exists") {
            if_exists = true;
        } else if (arg[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        } else if (name.empty()) {
            name = arg;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
            return 1;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }
    if (name.empty()) {
        std::fprintf(stderr, "error: database name is required\n");
        PrintUsage(argv[0]);
        return 1;
    }

    AdminResult r = DropDatabase(host, port, connect_db, name, if_exists);
    if (r != AdminResult::kOk) {
        std::fprintf(stderr, "dropdb: failed (code %d)\n", static_cast<int>(r));
        return 1;
    }
    std::fprintf(stdout, "DROP DATABASE\n");
    return 0;
}
