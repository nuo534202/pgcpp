// pg_checksums.cpp — pgcpp data page checksum verifier (pg_checksums main).
//
// Converted from PostgreSQL 15's src/bin/pg_checksums/.
//
// Scans all relation files under <data_dir>/base/<dboid>/<relfilenode> and
// verifies (or repairs) each page's 16-bit checksum.
//
// Usage:
//   pg_checksums -D <data_dir> [--check] [--enable] [--disable] [--verbose]
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "tools/pg_checksums.hpp"

using pgcpp::tools::ChecksumsMode;
using pgcpp::tools::ChecksumsOptions;
using pgcpp::tools::ChecksumsResult;
using pgcpp::tools::ChecksumsStats;
using pgcpp::tools::RunChecksums;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp data page checksum verifier (pg_checksums equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s -D <data_dir> [action] [options]\n\n", prog_name);
    std::fprintf(stderr, "Actions (one of):\n");
    std::fprintf(stderr, "  --check     Verify page checksums (default)\n");
    std::fprintf(stderr, "  --enable    Compute and write checksums on every page\n");
    std::fprintf(stderr, "  --disable   Clear checksums on every page\n");
    std::fprintf(stderr, "Options:\n");
    std::fprintf(stderr, "  -D <dir>    Data directory (PGDATA)\n");
    std::fprintf(stderr, "  --verbose   Print one line per scanned file\n");
    std::fprintf(stderr, "  --help      Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    ChecksumsOptions opts;
    bool show_help = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            show_help = true;
        } else if (arg == "-D" || arg == "--data-dir") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -D requires an argument\n");
                return 2;
            }
            opts.data_dir = argv[++i];
        } else if (arg == "--check") {
            opts.mode = ChecksumsMode::kCheck;
        } else if (arg == "--enable") {
            opts.mode = ChecksumsMode::kEnable;
        } else if (arg == "--disable") {
            opts.mode = ChecksumsMode::kDisable;
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    if (opts.data_dir.empty()) {
        std::fprintf(stderr, "error: -D <data_dir> is required\n");
        PrintUsage(argv[0]);
        return 2;
    }

    ChecksumsStats stats;
    ChecksumsResult result = RunChecksums(opts, std::cout, &stats);

    std::fprintf(stdout, "Scan summary:\n");
    std::fprintf(stdout, "  Files scanned:  %zu\n", stats.files_scanned);
    std::fprintf(stdout, "  Pages scanned:  %zu\n", stats.pages_scanned);
    std::fprintf(stdout, "  Bad checksums:  %zu\n", stats.bad_checksums);
    std::fprintf(stdout, "  Skipped (no checksum): %zu\n", stats.skipped);

    switch (result) {
        case ChecksumsResult::kOk:
            return 0;
        case ChecksumsResult::kOpenFailed:
            std::fprintf(stderr, "error: could not open data directory or relation file\n");
            return 1;
        case ChecksumsResult::kReadFailed:
            std::fprintf(stderr, "error: could not read a relation file\n");
            return 1;
        case ChecksumsResult::kChecksumMismatch:
            std::fprintf(stderr, "error: one or more page checksums did not match\n");
            return 1;
        case ChecksumsResult::kInvalidArgument:
            std::fprintf(stderr, "error: invalid arguments\n");
            return 2;
    }
    return 0;
}
