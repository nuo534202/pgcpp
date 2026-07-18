// pg_basebackup.cpp — main entry point for the pg_basebackup tool.
//
// Usage:
//   pgcpp_pg_basebackup [opts] -D <target_dir>
// Options:
//   -D, --pgdata=DIR           target data directory (required)
//   -X, --wal-method=METHOD    fetch (default), stream, none
//   -C, --checkpoint           force a checkpoint before backup (no-op locally)
//   -P, --progress             print progress
//   -v, --verbose              per-file output
//   --gzip                     gzip individual files (suffix .gz)
//   -Z, --compress=LEVEL      gzip compression level (1..9, default 6)
//   --src=DIR                  source data directory (defaults to PGDATA env)
//   -h, --host=HOST            server host (kept for parity, unused locally)
//   -p, --port=PORT            server port (kept for parity, unused locally)
//   -U, --user=USER             username
//   -d, --dbname=NAME          connection database
//   -n, --dry-run              list files without copying
#include <iostream>
#include <string>

#include "tools/pg_basebackup.hpp"

namespace {

void PrintUsage() {
    std::cerr << "Usage: pg_basebackup [opts] -D <target_dir>\n"
              << "  -D, --pgdata=DIR       target data directory\n"
              << "      --src=DIR          source data directory (default: $PGDATA)\n"
              << "  -X, --wal-method=M     fetch (default), stream, none\n"
              << "  -C, --checkpoint       force checkpoint (no-op locally)\n"
              << "  -P, --progress         print progress\n"
              << "  -v, --verbose          per-file output\n"
              << "      --gzip             gzip files (suffix .gz)\n"
              << "  -Z, --compress=N       gzip level (1..9, default 6)\n"
              << "  -n, --dry-run          list files without copying\n";
}

bool MatchOpt(const std::string& arg, const std::string& short_name,
              const std::string& long_name, std::string* value) {
    std::string short_eq = short_name + "=";
    std::string long_eq = long_name + "=";
    if (arg == short_name || arg == long_name) {
        return true;
    }
    if (arg.compare(0, short_eq.size(), short_eq) == 0) {
        *value = arg.substr(short_eq.size());
        return true;
    }
    if (arg.compare(0, long_eq.size(), long_eq) == 0) {
        *value = arg.substr(long_eq.size());
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    pgcpp::tools::BasebackupOptions opts;
    bool progress = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string val;
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (MatchOpt(a, "-D", "--pgdata", &val)) {
            if (!val.empty())
                opts.target_dir = val;
        } else if (MatchOpt(a, "", "--src", &val)) {
            if (!val.empty())
                opts.source_dir = val;
        } else if (MatchOpt(a, "-X", "--wal-method", &val)) {
            if (val == "fetch" || val.empty())
                opts.wal_method = pgcpp::tools::WalMethod::kFetch;
            else if (val == "stream")
                opts.wal_method = pgcpp::tools::WalMethod::kStream;
            else if (val == "none")
                opts.wal_method = pgcpp::tools::WalMethod::kNone;
            else {
                std::cerr << "unknown wal-method: " << val << "\n";
                return 2;
            }
        } else if (a == "-C" || a == "--checkpoint") {
            opts.checkpoint = true;
        } else if (a == "-P" || a == "--progress") {
            progress = true;
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--gzip") {
            opts.gzip = true;
        } else if (MatchOpt(a, "-Z", "--compress", &val)) {
            if (!val.empty()) {
                int lvl = std::stoi(val);
                if (lvl < 1) lvl = 1;
                if (lvl > 9) lvl = 9;
                opts.compression_level = lvl;
                opts.gzip = true;
            }
        } else if (a == "-n" || a == "--dry-run") {
            opts.dry_run = true;
        } else if (MatchOpt(a, "-h", "--host", &val)) {
            if (!val.empty())
                opts.host = val;
        } else if (MatchOpt(a, "-p", "--port", &val)) {
            if (!val.empty())
                opts.port = std::stoi(val);
        } else if (MatchOpt(a, "-U", "--user", &val)) {
            if (!val.empty())
                opts.user = val;
        } else if (MatchOpt(a, "-d", "--dbname", &val)) {
            if (!val.empty())
                opts.dbname = val;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (opts.target_dir.empty()) {
        std::cerr << "pg_basebackup: -D/--pgdata is required\n";
        return 2;
    }
    if (opts.source_dir.empty()) {
        const char* pgdata = std::getenv("PGDATA");
        if (pgdata)
            opts.source_dir = pgdata;
        else {
            std::cerr << "pg_basebackup: --src or $PGDATA is required\n";
            return 2;
        }
    }

    pgcpp::tools::BasebackupStats stats;
    std::ostream* vout = verbose ? &std::cout : nullptr;
    pgcpp::tools::BasebackupResult r = pgcpp::tools::RunBasebackup(opts, stats, vout);

    if (progress || verbose) {
        std::cout << "files copied: " << stats.files_copied
                  << ", skipped: " << stats.files_skipped
                  << ", bytes: " << pgcpp::tools::FormatBytes(stats.bytes_copied) << "\n";
    }

    switch (r) {
        case pgcpp::tools::BasebackupResult::kOk:
            return 0;
        case pgcpp::tools::BasebackupResult::kInvalidSourceDir:
            std::cerr << "pg_basebackup: invalid source data directory\n";
            return 1;
        case pgcpp::tools::BasebackupResult::kInvalidTargetDir:
            std::cerr << "pg_basebackup: invalid target directory\n";
            return 1;
        case pgcpp::tools::BasebackupResult::kSourceIsTarget:
            std::cerr << "pg_basebackup: source and target are the same\n";
            return 1;
        case pgcpp::tools::BasebackupResult::kCopyFailed:
            std::cerr << "pg_basebackup: copy failed\n";
            return 1;
        case pgcpp::tools::BasebackupResult::kNoSpaceLeft:
            std::cerr << "pg_basebackup: no space left\n";
            return 1;
    }
    return 0;
}
