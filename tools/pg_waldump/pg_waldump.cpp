// pg_waldump.cpp — pgcpp WAL record dumper (pg_waldump main entry).
//
// Converted from PostgreSQL 15's src/bin/pg_waldump/.
//
// Reads a WAL stream (in-memory buffer or file) and prints a one-line
// summary per XLogRecord.
//
// Usage:
//   pg_waldump [--start <lsn>] [--end <lsn>] [--path <wal_file>]
//              [--rmgr <name>] [--limit <n>] [--stats]
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "tools/pg_waldump.hpp"

using pgcpp::tools::DumpWal;
using pgcpp::tools::FormatLsn;
using pgcpp::tools::ParseLsn;
using pgcpp::tools::WaldumpOptions;
using pgcpp::tools::WaldumpResult;
using pgcpp::tools::WaldumpStats;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp WAL dumper (pg_waldump equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options]\n\n", prog_name);
    std::fprintf(stderr, "Options:\n");
    std::fprintf(stderr, "  --start <lsn>    Start reading at LSN (X/Y hex). Default: 0\n");
    std::fprintf(stderr, "  --end <lsn>      Stop reading at LSN (exclusive). Default: end of WAL\n");
    std::fprintf(stderr, "  --path <file>    Read WAL from <file> instead of the in-memory buffer\n");
    std::fprintf(stderr, "  --rmgr <name>    Only print records from this resource manager\n");
    std::fprintf(stderr, "  --limit <n>     Maximum number of records to print (0 = no limit)\n");
    std::fprintf(stderr, "  --stats         Print per-resource-manager statistics\n");
    std::fprintf(stderr, "  --help          Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    WaldumpOptions opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "--start") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --start requires an argument\n");
                return 2;
            }
            if (!ParseLsn(argv[++i], opts.start_lsn)) {
                std::fprintf(stderr, "error: invalid LSN for --start\n");
                return 2;
            }
        } else if (arg == "--end") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --end requires an argument\n");
                return 2;
            }
            if (!ParseLsn(argv[++i], opts.end_lsn)) {
                std::fprintf(stderr, "error: invalid LSN for --end\n");
                return 2;
            }
        } else if (arg == "--path") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --path requires an argument\n");
                return 2;
            }
            opts.path = argv[++i];
        } else if (arg == "--rmgr") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --rmgr requires an argument\n");
                return 2;
            }
            opts.rmgr_filter = argv[++i];
        } else if (arg == "--limit") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --limit requires an argument\n");
                return 2;
            }
            opts.limit = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--stats") {
            opts.stats = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    std::vector<WaldumpStats> stats;
    WaldumpResult result = DumpWal(opts, std::cout,
                                  opts.stats ? &stats : nullptr);

    switch (result) {
        case WaldumpResult::kOk:
            return 0;
        case WaldumpResult::kOpenFailed:
            std::fprintf(stderr, "error: could not open WAL file '%s'\n", opts.path.c_str());
            return 1;
        case WaldumpResult::kReadFailed:
            std::fprintf(stderr, "error: failed to read WAL record at LSN %s\n",
                        FormatLsn(opts.start_lsn).c_str());
            return 1;
        case WaldumpResult::kInvalidArgument:
            std::fprintf(stderr, "error: invalid rmgr name '%s'\n", opts.rmgr_filter.c_str());
            return 2;
    }
    return 0;
}
