// pg_basebackup.cpp — Base backup utility (pg_basebackup).
//
// Copies the source data directory (file-by-file) into the target directory,
// skipping runtime artifacts (postmaster.pid, log files, sockets). pg_wal/
// is copied only when --wal-method=fetch (default) and is excluded otherwise.
#include "tools/pg_basebackup.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace pgcpp::tools {

namespace {

// ReadDir — return the names of the entries in `path` (excluding "." and "..").
// Returns false on error.
bool ReadDir(const std::string& path, std::vector<std::string>& out) {
    DIR* d = opendir(path.c_str());
    if (!d)
        return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..")
            continue;
        out.push_back(std::move(name));
    }
    closedir(d);
    return true;
}

// IsDir — true if `path` is a directory (false for symlinks/files).
bool IsDir(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

// IsRegularFile — true if `path` is a regular file (follows symlinks).
bool IsRegularFile(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
}

// IsSymlink — true if `path` is a symbolic link.
bool IsSymlink(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return false;
    return S_ISLNK(st.st_mode);
}

// FileSize — size in bytes (0 on error).
std::int64_t FileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<std::int64_t>(st.st_size);
}

// CopyRegularFile — copy the contents of `src` to `dst`. If gzip is set,
// the destination is compressed (with `.gz` suffix appended to `dst`).
// Returns true on success.
bool CopyRegularFile(const std::string& src, const std::string& dst, bool gzip, int level,
                     std::int64_t* bytes_out) {
    std::ifstream in(src, std::ios::binary);
    if (!in)
        return false;
    std::int64_t total = 0;
    if (gzip) {
        std::string gz_path = dst + ".gz";
        gzFile gz_out = gzopen(gz_path.c_str(), "wb");
        if (!gz_out)
            return false;
        gzbuffer(gz_out, 64 * 1024);
        gzsetparams(gz_out, level, Z_DEFAULT_STRATEGY);
        char buf[64 * 1024];
        while (in) {
            in.read(buf, sizeof(buf));
            std::streamsize n = in.gcount();
            if (n > 0) {
                int w = gzwrite(gz_out, buf, static_cast<unsigned int>(n));
                if (w == 0) {
                    gzclose(gz_out);
                    return false;
                }
                total += n;
            }
        }
        gzclose(gz_out);
    } else {
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        char buf[64 * 1024];
        while (in) {
            in.read(buf, sizeof(buf));
            std::streamsize n = in.gcount();
            if (n > 0) {
                out.write(buf, n);
                total += n;
            }
        }
        if (!out) {
            return false;
        }
    }
    *bytes_out = total;
    return true;
}

// CopySymlink — recreate the symlink at `dst` pointing to the same target.
bool CopySymlink(const std::string& src, const std::string& dst) {
    char buf[4096];
    ssize_t n = readlink(src.c_str(), buf, sizeof(buf) - 1);
    if (n < 0)
        return false;
    buf[n] = '\0';
    if (unlink(dst.c_str()) != 0 && errno != ENOENT)
        return false;
    if (symlink(buf, dst.c_str()) != 0)
        return false;
    return true;
}

// CopyEntry — copy a single directory entry from `src_dir/name` to
// `dst_dir/name`, recursing into subdirectories.
bool CopyEntry(const std::string& src_dir, const std::string& dst_dir, const std::string& name,
               const BasebackupOptions& opts, BasebackupStats& stats, std::ostream* verbose_out);

// CopyDirRecursive — copy all entries from `src` into `dst` (creating `dst`
// if missing, unless dry_run is set). Honors the skip rules and the
// wal_method option.
bool CopyDirRecursive(const std::string& src, const std::string& dst, const BasebackupOptions& opts,
                      BasebackupStats& stats, std::ostream* verbose_out) {
    if (!opts.dry_run) {
        if (mkdir(dst.c_str(), 0700) != 0 && errno != EEXIST)
            return false;
    }
    std::vector<std::string> entries;
    if (!ReadDir(src, entries))
        return false;
    for (const auto& name : entries) {
        if (!CopyEntry(src, dst, name, opts, stats, verbose_out))
            return false;
    }
    return true;
}

// CopyEntry — copy a single directory entry from `src_dir/name` to
// `dst_dir/name`, recursing into subdirectories.
bool CopyEntry(const std::string& src_dir, const std::string& dst_dir, const std::string& name,
               const BasebackupOptions& opts, BasebackupStats& stats, std::ostream* verbose_out) {
    std::string src = src_dir + "/" + name;
    std::string dst = dst_dir + "/" + name;

    if (ShouldSkipFile(name)) {
        ++stats.files_skipped;
        stats.bytes_skipped += FileSize(src);
        if (verbose_out)
            *verbose_out << "skip " << name << "\n";
        return true;
    }

    // pg_wal/ handling.
    if (name == "pg_wal") {
        if (opts.wal_method == WalMethod::kNone) {
            ++stats.files_skipped;
            if (verbose_out)
                *verbose_out << "skip pg_wal (wal_method=none)\n";
            return true;
        }
        if (opts.wal_method == WalMethod::kFetch) {
            // Copy the entire directory.
            if (verbose_out)
                *verbose_out << "copy pg_wal/ (fetch)\n";
            return CopyDirRecursive(src, dst, opts, stats, verbose_out);
        }
        // kStream not supported — fall through to skip.
        ++stats.files_skipped;
        if (verbose_out)
            *verbose_out << "skip pg_wal (stream not supported)\n";
        return true;
    }

    if (opts.dry_run) {
        ++stats.files_copied;
        stats.bytes_copied += FileSize(src);
        if (verbose_out)
            *verbose_out << "would-copy " << name << "\n";
        return true;
    }

    if (IsSymlink(src)) {
        if (!CopySymlink(src, dst))
            return false;
        ++stats.files_copied;
        if (verbose_out)
            *verbose_out << "symlink " << name << "\n";
        return true;
    }
    if (IsDir(src)) {
        return CopyDirRecursive(src, dst, opts, stats, verbose_out);
    }
    if (IsRegularFile(src)) {
        std::int64_t bytes = 0;
        if (!CopyRegularFile(src, dst, opts.gzip, opts.compression_level, &bytes))
            return false;
        ++stats.files_copied;
        stats.bytes_copied += bytes;
        if (verbose_out)
            *verbose_out << "copy " << name << " (" << FormatBytes(bytes) << ")\n";
        return true;
    }
    // Unknown type — skip.
    ++stats.files_skipped;
    if (verbose_out)
        *verbose_out << "skip " << name << " (unknown type)\n";
    return true;
}

}  // namespace

bool IsDataDir(const std::string& path) {
    struct stat st;
    std::string pg_version = path + "/PG_VERSION";
    if (stat(pg_version.c_str(), &st) == 0 && S_ISREG(st.st_mode))
        return true;
    std::string conf = path + "/postgresql.conf";
    if (stat(conf.c_str(), &st) == 0 && S_ISREG(st.st_mode))
        return true;
    return false;
}

bool ShouldSkipFile(const std::string& basename) {
    if (basename == "postmaster.pid" || basename == "postmaster.opts")
        return true;
    // Log files (postmaster.log, *.log) under log/.
    if (basename.size() >= 4 && basename.compare(basename.size() - 4, 4, ".log") == 0)
        return true;
    // Socket files (pgsql.X.Y).
    if (basename.compare(0, 6, "pgsql.") == 0)
        return true;
    // Lock files (*.lock).
    if (basename.size() >= 5 && basename.compare(basename.size() - 5, 5, ".lock") == 0)
        return true;
    return false;
}

bool IsWalFile(const std::string& relpath) {
    // pg_wal/<segment> or pg_wal/archive_status/<...>
    if (relpath.compare(0, 7, "pg_wal/") != 0)
        return false;
    if (relpath == "pg_wal/archive_status")
        return true;
    if (relpath.compare(7, 14, "archive_status") == 0)
        return true;
    // Segment file names are 24 hex chars.
    if (relpath.size() == 7 + 24)
        return true;
    // .ready / .done files under archive_status.
    return false;
}

std::string FormatBytes(std::int64_t bytes) {
    constexpr std::int64_t kKiB = 1024;
    constexpr std::int64_t kMiB = 1024 * kKiB;
    constexpr std::int64_t kGiB = 1024 * kMiB;
    constexpr std::int64_t kTiB = 1024 * kGiB;
    char buf[64];
    if (bytes >= kTiB)
        std::snprintf(buf, sizeof(buf), "%.2f TiB", static_cast<double>(bytes) / kTiB);
    else if (bytes >= kGiB)
        std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / kGiB);
    else if (bytes >= kMiB)
        std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / kMiB);
    else if (bytes >= kKiB)
        std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(bytes) / kKiB);
    else
        std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    return std::string(buf);
}

bool EnsureTargetDir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode))
            return false;
        // Must be empty.
        std::vector<std::string> entries;
        if (!ReadDir(path, entries))
            return false;
        return entries.empty();
    }
    // Create (and parents if needed).
    std::string acc;
    for (std::size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        acc.push_back(c);
        if (c == '/' && i > 0) {
            if (mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
    }
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        return false;
    return true;
}

BasebackupResult RunBasebackup(const BasebackupOptions& opts, BasebackupStats& stats,
                               std::ostream* verbose_out) {
    if (opts.source_dir.empty() || opts.target_dir.empty())
        return BasebackupResult::kInvalidSourceDir;
    if (opts.source_dir == opts.target_dir)
        return BasebackupResult::kSourceIsTarget;

    if (!IsDataDir(opts.source_dir)) {
        if (verbose_out)
            *verbose_out << "source is not a data directory: " << opts.source_dir << "\n";
        return BasebackupResult::kInvalidSourceDir;
    }
    // For dry-run, validate but don't create the target directory.
    if (opts.dry_run) {
        struct stat st;
        if (stat(opts.target_dir.c_str(), &st) == 0) {
            // Target exists — must be a directory and empty for the dry-run
            // to make sense (matches the real-run check).
            if (!S_ISDIR(st.st_mode))
                return BasebackupResult::kInvalidTargetDir;
        }
        // If it doesn't exist, that's fine — we wouldn't be writing to it.
    } else if (!EnsureTargetDir(opts.target_dir)) {
        if (verbose_out)
            *verbose_out << "target dir is invalid or non-empty: " << opts.target_dir << "\n";
        return BasebackupResult::kInvalidTargetDir;
    }

    if (verbose_out)
        *verbose_out << "starting base backup: " << opts.source_dir << " -> " << opts.target_dir
                     << "\n";

    if (!CopyDirRecursive(opts.source_dir, opts.target_dir, opts, stats, verbose_out)) {
        if (verbose_out)
            *verbose_out << "copy failed\n";
        return BasebackupResult::kCopyFailed;
    }

    if (verbose_out)
        *verbose_out << "base backup complete: " << stats.files_copied << " files ("
                     << FormatBytes(stats.bytes_copied) << ")\n";
    return BasebackupResult::kOk;
}

}  // namespace pgcpp::tools
