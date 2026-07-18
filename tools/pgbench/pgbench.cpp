// pgbench.cpp — pgcpp TPC-B benchmark client (pgbench main entry).
//
// Converted from PostgreSQL 15's src/bin/pgbench/.
//
// Runs a simplified single-client TPC-B workload against a pgcpp server.
//
// Usage:
//   pgbench [-h host] [-p port] [-U user] [-d dbname] [-i] [-t N] [-s scale]
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "tools/pgbench.hpp"

using pgcpp::tools::PgbenchOptions;
using pgcpp::tools::PgbenchResult;
using pgcpp::tools::PgbenchStats;
using pgcpp::tools::RunPgbench;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp TPC-B benchmark client (pgbench equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options]\n\n", prog_name);
    std::fprintf(stderr, "Options:\n");
    std::fprintf(stderr, "  -h <host>     Server host (default: localhost)\n");
    std::fprintf(stderr, "  -p <port>     Server port (default: 5432)\n");
    std::fprintf(stderr, "  -U <user>     User name\n");
    std::fprintf(stderr, "  -d <dbname>   Database name (default: pgbench)\n");
    std::fprintf(stderr, "  -i            Initialize mode: create tables and populate\n");
    std::fprintf(stderr, "  -t <N>        Number of transactions per client (default: 10)\n");
    std::fprintf(stderr, "  -s <scale>    Scale factor (default: 1)\n");
    std::fprintf(stderr, "  --seed <N>    Random seed (default: time)\n");
    std::fprintf(stderr, "  --help        Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    PgbenchOptions opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "-h") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -h requires an argument\n");
                return 2;
            }
            opts.host = argv[++i];
        } else if (arg == "-p") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -p requires an argument\n");
                return 2;
            }
            opts.port = std::atoi(argv[++i]);
        } else if (arg == "-U") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -U requires an argument\n");
                return 2;
            }
            opts.user = argv[++i];
        } else if (arg == "-d" || arg == "--dbname") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -d requires an argument\n");
                return 2;
            }
            opts.dbname = argv[++i];
        } else if (arg == "-i") {
            opts.initialize = true;
        } else if (arg == "-t") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -t requires an argument\n");
                return 2;
            }
            opts.transactions = std::atoi(argv[++i]);
        } else if (arg == "-s") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -s requires an argument\n");
                return 2;
            }
            opts.scale = std::atoi(argv[++i]);
        } else if (arg == "--seed") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --seed requires an argument\n");
                return 2;
            }
            opts.seed = static_cast<unsigned int>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    PgbenchStats stats;
    PgbenchResult result = RunPgbench(opts, std::cout, &stats);

    switch (result) {
        case PgbenchResult::kOk:
            return 0;
        case PgbenchResult::kConnectFailed:
            std::fprintf(stderr, "error: could not connect to server\n");
            return 1;
        case PgbenchResult::kInitFailed:
            std::fprintf(stderr, "error: initialization failed\n");
            return 1;
        case PgbenchResult::kTransactionFailed:
            std::fprintf(stderr, "error: %d transactions failed\n", stats.transactions_failed);
            return 1;
        case PgbenchResult::kInvalidArgument:
            std::fprintf(stderr, "error: invalid arguments (transactions and scale must be > 0)\n");
            return 2;
    }
    return 0;
}
