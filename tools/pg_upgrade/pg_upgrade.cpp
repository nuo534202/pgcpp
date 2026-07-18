// pg_upgrade.cpp — main entry point for the pg_upgrade tool.
//
// Usage:
//   pgcpp_pg_upgrade [opts] -d <old> -D <new>
// Options:
//   -d, --old-datadir=DIR    old cluster data directory
//   -D, --new-datadir=DIR    new cluster data directory
//   -k, --link               use hard links (fastest; same filesystem)
//       --clone              use reflinks (reflink-capable filesystem)
//   -c, --check              only check compatibility, don't migrate
//   -v, --verbose            per-file output
//   -j, --jobs=N            parallel jobs (ignored by pgcpp, single-threaded)
#include <iostream>
#include <string>

#include "tools/pg_upgrade.hpp"

namespace {

void PrintUsage() {
    std::cerr << "Usage: pg_upgrade [opts] -d <old> -D <new>\n"
              << "  -d, --old-datadir=DIR   old cluster data directory\n"
              << "  -D, --new-datadir=DIR   new cluster data directory\n"
              << "  -k, --link              use hard links\n"
              << "      --clone             use reflinks\n"
              << "  -c, --check             check only, no migration\n"
              << "  -v, --verbose           per-file output\n"
              << "  -j, --jobs=N            parallel jobs (ignored)\n";
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
    pgcpp::tools::UpgradeOptions opts;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string val;
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (MatchOpt(a, "-d", "--old-datadir", &val)) {
            if (!val.empty())
                opts.old_dir = val;
        } else if (MatchOpt(a, "-D", "--new-datadir", &val)) {
            if (!val.empty())
                opts.new_dir = val;
        } else if (a == "-k" || a == "--link") {
            opts.mode = pgcpp::tools::UpgradeMode::kLink;
        } else if (a == "--clone") {
            opts.mode = pgcpp::tools::UpgradeMode::kClone;
        } else if (a == "-c" || a == "--check") {
            opts.check_only = true;
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (MatchOpt(a, "-j", "--jobs", &val)) {
            if (!val.empty()) {
                int j = std::stoi(val);
                if (j < 1) j = 1;
                opts.jobs = j;
            }
        } else {
            std::cerr << "unknown option: " << a << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (opts.old_dir.empty() || opts.new_dir.empty()) {
        std::cerr << "pg_upgrade: -d/--old-datadir and -D/--new-datadir are required\n";
        return 2;
    }

    pgcpp::tools::UpgradeStats stats;
    std::ostream* vout = verbose ? &std::cout : nullptr;
    pgcpp::tools::UpgradeResult r = pgcpp::tools::RunUpgrade(opts, stats, vout);

    if (verbose && !opts.check_only) {
        std::cout << "files copied: " << stats.files_copied
                  << ", linked: " << stats.files_linked
                  << ", cloned: " << stats.files_cloned
                  << ", skipped: " << stats.files_skipped << "\n";
    }

    switch (r) {
        case pgcpp::tools::UpgradeResult::kOk:
            return 0;
        case pgcpp::tools::UpgradeResult::kInvalidOldDir:
            std::cerr << "pg_upgrade: invalid old data directory\n";
            return 1;
        case pgcpp::tools::UpgradeResult::kInvalidNewDir:
            std::cerr << "pg_upgrade: invalid new data directory\n";
            return 1;
        case pgcpp::tools::UpgradeResult::kSameDirectory:
            std::cerr << "pg_upgrade: old and new are the same\n";
            return 1;
        case pgcpp::tools::UpgradeResult::kVersionMismatch:
            std::cerr << "pg_upgrade: version mismatch\n";
            return 1;
        case pgcpp::tools::UpgradeResult::kNewClusterNotEmpty:
            std::cerr << "pg_upgrade: new cluster already has user data\n";
            return 1;
        case pgcpp::tools::UpgradeResult::kOldClusterRunning:
            std::cerr << "pg_upgrade: old cluster is still running\n";
            return 1;
        case pgcpp::tools::UpgradeResult::kCopyFailed:
            std::cerr << "pg_upgrade: copy failed\n";
            return 1;
        case pgcpp::tools::UpgradeResult::kCloneUnsupported:
            std::cerr << "pg_upgrade: clone not supported\n";
            return 1;
    }
    return 0;
}
