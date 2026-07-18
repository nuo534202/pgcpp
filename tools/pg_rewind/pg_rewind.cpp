// pg_rewind.cpp — main entry point for the pg_rewind tool.
//
// Usage:
//   pgcpp_pg_rewind [opts] --target-pgdata=DIR --source-pgdata=DIR
// Options:
//   -D, --target-pgdata=DIR    the data directory to rewind (required)
//       --source-pgdata=DIR    the source data directory to copy from
//       --source-server=CONN   connection string to a source server
//                              (kept for parity; pgcpp doesn't speak the
//                              replication protocol, so this is mapped to
//                              --source-pgdata=local-path)
//   -n, --dry-run              don't actually modify target
//   -v, --verbose              per-file output
//   -q, --quick                compare file sizes only (skip content hash)
//       --no-sync              don't fsync target files (parity flag)
#include <iostream>
#include <string>

#include "tools/pg_rewind.hpp"

namespace {

void PrintUsage() {
    std::cerr << "Usage: pg_rewind [opts] -D <target> --source-pgdata=<source>\n"
              << "  -D, --target-pgdata=DIR    target data directory\n"
              << "      --source-pgdata=DIR    source data directory\n"
              << "      --source-server=CONN   (not supported; use --source-pgdata)\n"
              << "  -n, --dry-run              no modifications\n"
              << "  -v, --verbose              per-file output\n"
              << "  -q, --quick                compare sizes only\n"
              << "      --no-sync              skip fsync (parity)\n";
}

bool MatchOpt(const std::string& arg, const std::string& short_name,
              const std::string& long_name, std::string* value) {
    std::string short_eq = short_name + "=";
    std::string long_eq = long_name + "=";
    if (arg == short_name || arg == long_name) {
        return true;
    }
    if (!short_eq.empty() && arg.compare(0, short_eq.size(), short_eq) == 0) {
        *value = arg.substr(short_eq.size());
        return true;
    }
    if (!long_eq.empty() && arg.compare(0, long_eq.size(), long_eq) == 0) {
        *value = arg.substr(long_eq.size());
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    pgcpp::tools::RewindOptions opts;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string val;
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (MatchOpt(a, "-D", "--target-pgdata", &val)) {
            if (!val.empty())
                opts.target_dir = val;
        } else if (MatchOpt(a, "", "--source-pgdata", &val)) {
            if (!val.empty())
                opts.source_dir = val;
        } else if (MatchOpt(a, "", "--source-server", &val)) {
            // Not supported — print a hint and exit non-zero.
            std::cerr << "pg_rewind: --source-server is not supported; use --source-pgdata\n";
            return 2;
        } else if (a == "-n" || a == "--dry-run") {
            opts.dry_run = true;
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "-q" || a == "--quick") {
            opts.quick = true;
        } else if (a == "--no-sync") {
            opts.no_sync = true;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (opts.target_dir.empty() || opts.source_dir.empty()) {
        std::cerr << "pg_rewind: --target-pgdata and --source-pgdata are required\n";
        return 2;
    }

    pgcpp::tools::RewindStats stats;
    std::ostream* vout = verbose ? &std::cout : nullptr;
    pgcpp::tools::RewindResult r = pgcpp::tools::RunRewind(opts, stats, vout);

    if (verbose) {
        std::cout << "copied: " << stats.files_copied
                  << ", removed: " << stats.files_removed
                  << ", unchanged: " << stats.files_unchanged << "\n";
    }

    switch (r) {
        case pgcpp::tools::RewindResult::kOk:
            return 0;
        case pgcpp::tools::RewindResult::kInvalidSourceDir:
            std::cerr << "pg_rewind: invalid source data directory\n";
            return 1;
        case pgcpp::tools::RewindResult::kInvalidTargetDir:
            std::cerr << "pg_rewind: invalid target data directory\n";
            return 1;
        case pgcpp::tools::RewindResult::kSourceIsTarget:
            std::cerr << "pg_rewind: source and target are the same\n";
            return 1;
        case pgcpp::tools::RewindResult::kSourceNotNewer:
            std::cerr << "pg_rewind: source is not newer than target\n";
            return 1;
        case pgcpp::tools::RewindResult::kCopyFailed:
            std::cerr << "pg_rewind: copy failed\n";
            return 1;
        case pgcpp::tools::RewindResult::kRemoveFailed:
            std::cerr << "pg_rewind: remove failed\n";
            return 1;
    }
    return 0;
}
