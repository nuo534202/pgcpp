// clusterdb.cpp — pgcpp CLUSTER utility (clusterdb equivalent).
//
// Converted from PostgreSQL 15's src/bin/clusterdb/.
//
// Connects to a running server and issues a CLUSTER command.
//
// Usage:
//   clusterdb [-h host] [-p port] [-d database]
//             [--verbose] [-t table] [-i index]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "tools/sql_admin.hpp"

using pgcpp::tools::AdminResult;
using pgcpp::tools::ClusterDatabase;
using pgcpp::tools::ClusterOptions;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp cluster utility (clusterdb equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options]\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>          Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>          Server port (default: 5433)\n");
    std::fprintf(stderr, "  -d <database>      Database (default: pgcpp)\n");
    std::fprintf(stderr, "  -t <table>         Table to cluster\n");
    std::fprintf(stderr, "  -i <index>         Index to use for clustering\n");
    std::fprintf(stderr, "  --verbose          VERBOSE output\n");
    std::fprintf(stderr, "  --help             Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    ClusterOptions opts;
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
        } else if (arg == "-t" || arg == "--table") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -t requires an argument\n");
                return 1;
            }
            opts.table = argv[++i];
        } else if (arg == "-i" || arg == "--index") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -i requires an argument\n");
                return 1;
            }
            opts.index = argv[++i];
        } else if (arg == "--verbose") {
            opts.verbose = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    AdminResult r = ClusterDatabase(host, port, database, opts);
    if (r != AdminResult::kOk) {
        std::fprintf(stderr, "clusterdb: failed (code %d)\n", static_cast<int>(r));
        return 1;
    }
    std::fprintf(stdout, "CLUSTER\n");
    return 0;
}
