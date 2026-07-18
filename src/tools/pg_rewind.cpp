// pg_rewind.cpp — Data directory synchronizer (pg_rewind).
//
// Walks source and target data directories, computes a relative-path list
// of each, then synchronizes the target to match the source by:
//   - copying files that differ or are new to source
//   - removing files that no longer exist in source
// Runtime artifacts are skipped on both sides.
#include "tools/pg_rewind.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "tools/pg_basebackup.hpp"  // IsDataDir, ShouldSkipFile, FormatBytes

namespace pgcpp::tools {

namespace {

bool IsDir(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

bool IsSymlink(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return false;
    return S_ISLNK(st.st_mode);
}

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

std::int64_t FileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return -1;
    return static_cast<std::int64_t>(st.st_size);
}

bool CopyRegularFile(const std::string& src, const std::string& dst, std::int64_t* bytes_out) {
    std::ifstream in(src, std::ios::binary);
    if (!in)
        return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    char buf[64 * 1024];
    std::int64_t total = 0;
    while (in) {
        in.read(buf, sizeof(buf));
        std::streamsize n = in.gcount();
        if (n > 0) {
            out.write(buf, n);
            total += n;
        }
    }
    if (!out)
        return false;
    *bytes_out = total;
    return true;
}

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

// WalkDir — recursive walk. Appends `relpath` entries (relative to `root`)
// to `out`.
void WalkDir(const std::string& root, const std::string& relpath, std::vector<std::string>& out) {
    std::string abs_dir = relpath.empty() ? root : (root + "/" + relpath);
    std::vector<std::string> entries;
    if (!ReadDir(abs_dir, entries))
        return;
    std::sort(entries.begin(), entries.end());
    for (const auto& name : entries) {
        if (ShouldSkipFile(name))
            continue;
        std::string abs_path = abs_dir + "/" + name;
        std::string rel_path = relpath.empty() ? name : (relpath + "/" + name);
        if (IsSymlink(abs_path) || !IsDir(abs_path)) {
            out.push_back(rel_path);
        } else {
            // Recurse — include the directory entry itself so empty dirs
            // can be removed if they're gone from source.
            out.push_back(rel_path + "/");
            WalkDir(root, rel_path, out);
        }
    }
}

}  // namespace

std::uint64_t ComputeFileHash(const std::string& path) {
    // FNV-1a 64-bit.
    std::uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return 0;
    char buf[64 * 1024];
    while (in) {
        in.read(buf, sizeof(buf));
        std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            hash ^= static_cast<unsigned char>(buf[i]);
            hash *= 1099511628211ULL;  // FNV prime
        }
    }
    return hash;
}

bool FilesDiffer(const std::string& path1, const std::string& path2, bool quick) {
    std::int64_t s1 = FileSize(path1);
    std::int64_t s2 = FileSize(path2);
    if (s1 < 0 || s2 < 0)
        return true;
    if (s1 != s2)
        return true;
    if (quick)
        return false;
    return ComputeFileHash(path1) != ComputeFileHash(path2);
}

std::vector<std::string> EnumerateDataDirFiles(const std::string& path) {
    std::vector<std::string> out;
    WalkDir(path, "", out);
    return out;
}

bool SyncFile(const std::string& src, const std::string& dst, std::int64_t* bytes_out) {
    if (!EnsureParentDir(dst))
        return false;
    if (IsSymlink(src))
        return CopySymlink(src, dst);
    // Remove dst if it exists and is not a regular file (e.g. a dir).
    struct stat st;
    if (lstat(dst.c_str(), &st) == 0 && !S_ISREG(st.st_mode)) {
        if (unlink(dst.c_str()) != 0)
            return false;
    }
    return CopyRegularFile(src, dst, bytes_out);
}

bool RemoveFile(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;  // already gone is OK
    }
    if (S_ISDIR(st.st_mode)) {
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

bool EnsureParentDir(const std::string& path) {
    std::string parent;
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return true;
    parent = path.substr(0, slash);
    if (parent.empty())
        return true;
    // mkdir -p
    std::string acc;
    for (std::size_t i = 0; i < parent.size(); ++i) {
        acc.push_back(parent[i]);
        if (parent[i] == '/' && i > 0) {
            if (mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
    }
    if (mkdir(parent.c_str(), 0700) != 0 && errno != EEXIST)
        return false;
    return true;
}

RewindResult RunRewind(const RewindOptions& opts, RewindStats& stats, std::ostream* verbose_out) {
    if (opts.source_dir.empty() || opts.target_dir.empty())
        return RewindResult::kInvalidSourceDir;
    if (opts.source_dir == opts.target_dir)
        return RewindResult::kSourceIsTarget;
    if (!pgcpp::tools::IsDataDir(opts.source_dir)) {
        if (verbose_out)
            *verbose_out << "source is not a data directory: " << opts.source_dir << "\n";
        return RewindResult::kInvalidSourceDir;
    }
    if (!pgcpp::tools::IsDataDir(opts.target_dir)) {
        if (verbose_out)
            *verbose_out << "target is not a data directory: " << opts.target_dir << "\n";
        return RewindResult::kInvalidTargetDir;
    }

    if (verbose_out)
        *verbose_out << "rewinding: " << opts.target_dir << " <- " << opts.source_dir << "\n";

    std::vector<std::string> src_files = EnumerateDataDirFiles(opts.source_dir);
    std::vector<std::string> tgt_files = EnumerateDataDirFiles(opts.target_dir);

    std::unordered_set<std::string> src_set(src_files.begin(), src_files.end());
    std::unordered_set<std::string> tgt_set(tgt_files.begin(), tgt_files.end());

    // Phase 1: copy/keep files that exist in source.
    for (const auto& rel : src_files) {
        std::string src_path = opts.source_dir + "/" + rel;
        std::string tgt_path = opts.target_dir + "/" + rel;

        // Skip directory entries (handled by walking files only).
        if (!rel.empty() && rel.back() == '/')
            continue;

        bool in_target = tgt_set.count(rel) > 0;
        if (!in_target) {
            // New file in source — copy to target.
            if (opts.dry_run) {
                ++stats.files_copied;
                if (verbose_out)
                    *verbose_out << "would-add " << rel << "\n";
                continue;
            }
            std::int64_t bytes = 0;
            if (!SyncFile(src_path, tgt_path, &bytes)) {
                ++stats.errors;
                if (verbose_out)
                    *verbose_out << "ERROR adding " << rel << "\n";
                return RewindResult::kCopyFailed;
            }
            ++stats.files_copied;
            stats.bytes_copied += bytes;
            if (verbose_out)
                *verbose_out << "add " << rel << " (" << FormatBytes(bytes) << ")\n";
        } else {
            // Exists in both — compare and copy if differ.
            if (FilesDiffer(src_path, tgt_path, opts.quick)) {
                if (opts.dry_run) {
                    ++stats.files_copied;
                    if (verbose_out)
                        *verbose_out << "would-update " << rel << "\n";
                    continue;
                }
                std::int64_t bytes = 0;
                if (!SyncFile(src_path, tgt_path, &bytes)) {
                    ++stats.errors;
                    if (verbose_out)
                        *verbose_out << "ERROR updating " << rel << "\n";
                    return RewindResult::kCopyFailed;
                }
                ++stats.files_copied;
                stats.bytes_copied += bytes;
                if (verbose_out)
                    *verbose_out << "update " << rel << " (" << FormatBytes(bytes) << ")\n";
            } else {
                ++stats.files_unchanged;
                if (verbose_out)
                    *verbose_out << "keep " << rel << "\n";
            }
        }
    }

    // Phase 2: remove files that exist only in target.
    for (const auto& rel : tgt_files) {
        if (!rel.empty() && rel.back() == '/')
            continue;
        if (src_set.count(rel) > 0)
            continue;
        std::string tgt_path = opts.target_dir + "/" + rel;
        if (opts.dry_run) {
            ++stats.files_removed;
            if (verbose_out)
                *verbose_out << "would-remove " << rel << "\n";
            continue;
        }
        if (!RemoveFile(tgt_path)) {
            ++stats.errors;
            if (verbose_out)
                *verbose_out << "ERROR removing " << rel << "\n";
            return RewindResult::kRemoveFailed;
        }
        ++stats.files_removed;
        if (verbose_out)
            *verbose_out << "remove " << rel << "\n";
    }

    if (verbose_out)
        *verbose_out << "rewind complete: " << stats.files_copied << " copied, "
                     << stats.files_removed << " removed, " << stats.files_unchanged
                     << " unchanged\n";
    return RewindResult::kOk;
}

}  // namespace pgcpp::tools
