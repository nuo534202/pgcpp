// reindexdb.cpp — pgcpp REINDEX utility (reindexdb equivalent).
//
// Converted from PostgreSQL 15's src/bin/reindexdb/.
//
// Connects to a running server and issues a REINDEX command.
//
// Usage:
//   reindexdb [-h host] [-p port] [-d database]
//              [--index|--table|--database|--system] [name]
//              [--concurrently] [--verbose]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "tools/sql_admin.hpp"

using pgcpp::tools::AdminResult;
using pgcpp::tools::ReindexDatabase;
using pgcpp::tools::ReindexOptions;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp reindex utility (reindexdb equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options] [name]\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>          Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>          Server port (default: 5433)\n");
    std::fprintf(stderr, "  -d <database>      Database (default: pgcpp)\n");
    std::fprintf(stderr, "  --index            Reindex a specific index\n");
    std::fprintf(stderr, "  --table            Reindex a specific table\n");
    std::fprintf(stderr, "  --database         Reindex the entire database (default)\n");
    std::fprintf(stderr, "  --system           Reindex system catalogs\n");
    std::fprintf(stderr, "  --concurrently     Use CONCURRENTLY\n");
    std::fprintf(stderr, "  --verbose          VERBOSE output\n");
    std::fprintf(stderr, "  --help             Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    ReindexOptions opts;
    opts.kind = ReindexOptions::Kind::kDatabase;
    std::string host = "127.0.0.1";
    int port = 5433;
    std::string database = "pgcpp";
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
            database = argv[++i];
        } else if (arg == "--index") {
            opts.kind = ReindexOptions::Kind::kIndex;
        } else if (arg == "--table") {
            opts.kind = ReindexOptions::Kind::kTable;
        } else if (arg == "--database") {
            opts.kind = ReindexOptions::Kind::kDatabase;
        } else if (arg == "--system") {
            opts.kind = ReindexOptions::Kind::kSystem;
        } else if (arg == "--concurrently") {
            opts.concurrently = true;
        } else if (arg == "--verbose") {
            opts.verbose = true;
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

    AdminResult r = ReindexDatabase(host, port, database, opts);
    if (r != AdminResult::kOk) {
        std::fprintf(stderr, "reindexdb: failed (code %d)\n", static_cast<int>(r));
        return 1;
    }
    std::fprintf(stdout, "REINDEX\n");
    return 0;
}
