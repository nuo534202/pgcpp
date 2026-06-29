// createdb.cpp — pgcpp CREATE DATABASE utility (createdb equivalent).
//
// Converted from PostgreSQL 15's src/bin/createdb/.
//
// Connects to a running server and issues a CREATE DATABASE statement.
//
// Usage:
//   createdb [-h host] [-p port] [-d connect_db] <name>
//            [-O owner] [-T template] [-E encoding]
//            [--lc-collate=...] [--lc-ctype=...]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pgcpp/tools/sql_admin.hpp"

using pgcpp::tools::AdminResult;
using pgcpp::tools::CreateDatabase;
using pgcpp::tools::CreatedbOptions;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp create-database utility (createdb equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options] <dbname>\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>          Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>          Server port (default: 5433)\n");
    std::fprintf(stderr, "  -d <database>      Database to connect to (default: pgcpp)\n");
    std::fprintf(stderr, "  -O <owner>         Database owner\n");
    std::fprintf(stderr, "  -T <template>      Template database\n");
    std::fprintf(stderr, "  -E <encoding>      Encoding\n");
    std::fprintf(stderr, "  --lc-collate=<s>   LC_COLLATE\n");
    std::fprintf(stderr, "  --lc-ctype=<s>     LC_CTYPE\n");
    std::fprintf(stderr, "  --help             Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    CreatedbOptions opts;
    std::string host = "127.0.0.1";
    int port = 5433;
    std::string connect_db = "pgcpp";
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
        } else if (arg == "-O" || arg == "--owner") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -O requires an argument\n");
                return 1;
            }
            opts.owner = argv[++i];
        } else if (arg == "-T" || arg == "--template") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -T requires an argument\n");
                return 1;
            }
            opts.template_db = argv[++i];
        } else if (arg == "-E" || arg == "--encoding") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -E requires an argument\n");
                return 1;
            }
            opts.encoding = argv[++i];
        } else if (arg.compare(0, 13, "--lc-collate=") == 0) {
            opts.lc_collate = arg.substr(13);
        } else if (arg.compare(0, 11, "--lc-ctype=") == 0) {
            opts.lc_ctype = arg.substr(11);
        } else if (arg[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        } else if (opts.name.empty()) {
            opts.name = arg;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
            return 1;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    if (opts.name.empty()) {
        std::fprintf(stderr, "error: database name is required\n");
        PrintUsage(argv[0]);
        return 1;
    }

    AdminResult r = CreateDatabase(host, port, connect_db, opts);
    if (r != AdminResult::kOk) {
        std::fprintf(stderr, "createdb: failed (code %d)\n", static_cast<int>(r));
        return 1;
    }
    std::fprintf(stdout, "CREATE DATABASE\n");
    return 0;
}
