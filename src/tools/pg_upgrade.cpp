// pg_upgrade.cpp — Cluster upgrade utility (pg_upgrade).
//
// Verifies two data directories' PG_VERSION files, then (unless --check)
// copies relation files from old/base/<db>/ to new/base/<db>/ using the
// selected mode (copy/link/clone). System catalog files are NOT copied —
// they are managed by the new cluster's initdb.
#include "tools/pg_upgrade.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
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
#include <sstream>
#include <string>
#include <vector>

#include "tools/pg_basebackup.hpp"  // IsDataDir, ShouldSkipFile

namespace pgcpp::tools {

namespace {

bool IsDir(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
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

// CopyRegularFile — straightforward content copy.
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

// HardLinkFile — create a hard link from src to dst. Removes dst if it exists.
bool HardLinkFile(const std::string& src, const std::string& dst) {
    if (unlink(dst.c_str()) != 0 && errno != ENOENT)
        return false;
    return link(src.c_str(), dst.c_str()) == 0;
}

// CloneFile — Linux FICLONE (reflink). Returns false with errno=EXDEV or
// ENOTSUP if the filesystem doesn't support reflinks.
bool CloneFile(const std::string& src, const std::string& dst) {
    if (unlink(dst.c_str()) != 0 && errno != ENOENT)
        return false;
    int sfd = open(src.c_str(), O_RDONLY);
    if (sfd < 0)
        return false;
    int dfd = open(dst.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
    if (dfd < 0) {
        close(sfd);
        return false;
    }
    int rc = ioctl(dfd, FICLONE, sfd);
    int saved_errno = errno;
    close(sfd);
    close(dfd);
    if (rc != 0) {
        errno = saved_errno;
        // Remove the (likely empty) destination.
        unlink(dst.c_str());
        return false;
    }
    return true;
}

bool EnsureParentDir(const std::string& path) {
    std::string parent;
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return true;
    parent = path.substr(0, slash);
    if (parent.empty())
        return true;
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

bool Mkdir(const std::string& path) {
    return mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

}  // namespace

int ReadVersionFile(const std::string& dir) {
    std::string path = dir + "/PG_VERSION";
    std::ifstream in(path);
    if (!in)
        return 0;
    int v = 0;
    in >> v;
    if (!in)
        return 0;
    return v;
}

bool CheckCompatibility(int old_version, int new_version) {
    // pgcpp only supports same-major-version "upgrades" (no-op).
    return old_version > 0 && old_version == new_version;
}

bool IsClusterRunning(const std::string& dir) {
    std::string path = dir + "/postmaster.pid";
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool NewClusterHasUserData(const std::string& dir) {
    std::string base = dir + "/base";
    std::vector<std::string> db_dirs;
    if (!ReadDir(base, db_dirs))
        return false;
    for (const auto& db : db_dirs) {
        std::string db_path = base + "/" + db;
        if (!IsDir(db_path))
            continue;
        std::vector<std::string> rels;
        if (!ReadDir(db_path, rels))
            continue;
        for (const auto& rel : rels) {
            if (!IsRelationFile(rel))
                continue;
            if (FileSize(db_path + "/" + rel) > 0)
                return true;
        }
    }
    return false;
}

bool CopyRelationFile(const std::string& src, const std::string& dst, UpgradeMode mode,
                      std::int64_t* bytes_out) {
    if (!EnsureParentDir(dst))
        return false;
    if (mode == UpgradeMode::kLink) {
        if (!HardLinkFile(src, dst))
            return false;
        if (bytes_out)
            *bytes_out = FileSize(src);
        return true;
    }
    if (mode == UpgradeMode::kClone) {
        if (!CloneFile(src, dst)) {
            // Caller should retry with kCopy.
            errno = errno == 0 ? ENOTSUP : errno;
            return false;
        }
        if (bytes_out)
            *bytes_out = FileSize(src);
        return true;
    }
    // kCopy
    return CopyRegularFile(src, dst, bytes_out);
}

bool IsRelationFile(const std::string& basename) {
    // Numeric name, optional _fsm / _vm / _init suffix.
    if (basename.empty())
        return false;
    std::size_t i = 0;
    while (i < basename.size() && std::isdigit(static_cast<unsigned char>(basename[i])))
        ++i;
    if (i == 0)
        return false;  // no leading digits
    if (i == basename.size())
        return true;
    // Optional suffix.
    static const char* kSuffixes[] = {"_fsm", "_vm", "_init"};
    for (const char* s : kSuffixes) {
        std::size_t sl = std::strlen(s);
        if (basename.size() - i == sl && basename.compare(i, sl, s) == 0)
            return true;
    }
    return false;
}

UpgradeResult RunUpgrade(const UpgradeOptions& opts, UpgradeStats& stats,
                         std::ostream* verbose_out) {
    if (opts.old_dir.empty() || opts.new_dir.empty())
        return UpgradeResult::kInvalidOldDir;
    if (opts.old_dir == opts.new_dir)
        return UpgradeResult::kSameDirectory;
    if (!pgcpp::tools::IsDataDir(opts.old_dir)) {
        if (verbose_out)
            *verbose_out << "old is not a data directory: " << opts.old_dir << "\n";
        return UpgradeResult::kInvalidOldDir;
    }
    if (!pgcpp::tools::IsDataDir(opts.new_dir)) {
        if (verbose_out)
            *verbose_out << "new is not a data directory: " << opts.new_dir << "\n";
        return UpgradeResult::kInvalidNewDir;
    }

    int old_v = ReadVersionFile(opts.old_dir);
    int new_v = ReadVersionFile(opts.new_dir);
    if (verbose_out)
        *verbose_out << "old version: " << old_v << ", new version: " << new_v << "\n";
    if (old_v == 0 || new_v == 0) {
        if (verbose_out)
            *verbose_out << "missing or unreadable PG_VERSION\n";
        return old_v == 0 ? UpgradeResult::kInvalidOldDir : UpgradeResult::kInvalidNewDir;
    }
    if (!CheckCompatibility(old_v, new_v)) {
        if (verbose_out)
            *verbose_out << "incompatible versions: " << old_v << " -> " << new_v << "\n";
        return UpgradeResult::kVersionMismatch;
    }

    if (IsClusterRunning(opts.old_dir)) {
        if (verbose_out)
            *verbose_out << "old cluster is still running (postmaster.pid present)\n";
        return UpgradeResult::kOldClusterRunning;
    }
    if (!opts.check_only && NewClusterHasUserData(opts.new_dir)) {
        if (verbose_out)
            *verbose_out << "new cluster already has user data\n";
        return UpgradeResult::kNewClusterNotEmpty;
    }

    if (opts.check_only) {
        if (verbose_out)
            *verbose_out << "check complete: compatible\n";
        return UpgradeResult::kOk;
    }

    // Walk base/<db>/<rel> in old and copy/link/clone to new.
    std::string old_base = opts.old_dir + "/base";
    std::string new_base = opts.new_dir + "/base";
    std::vector<std::string> db_dirs;
    if (!ReadDir(old_base, db_dirs)) {
        if (verbose_out)
            *verbose_out << "no base/ directory in old cluster\n";
        // No user data to migrate — that's a valid upgrade (no-op).
        return UpgradeResult::kOk;
    }
    for (const auto& db : db_dirs) {
        std::string old_db = old_base + "/" + db;
        std::string new_db = new_base + "/" + db;
        if (!IsDir(old_db))
            continue;
        if (!Mkdir(new_db))
            return UpgradeResult::kCopyFailed;
        std::vector<std::string> rels;
        if (!ReadDir(old_db, rels))
            continue;
        for (const auto& rel : rels) {
            if (!IsRelationFile(rel)) {
                ++stats.files_skipped;
                continue;
            }
            std::string src = old_db + "/" + rel;
            std::string dst = new_db + "/" + rel;
            std::int64_t bytes = 0;
            if (!CopyRelationFile(src, dst, opts.mode, &bytes)) {
                // For kClone, fall back to kCopy.
                if (opts.mode == UpgradeMode::kClone) {
                    if (verbose_out)
                        *verbose_out << "clone unsupported for " << rel
                                     << ", falling back to copy\n";
                    if (!CopyRelationFile(src, dst, UpgradeMode::kCopy, &bytes)) {
                        ++stats.errors;
                        if (verbose_out)
                            *verbose_out << "ERROR copying " << rel << "\n";
                        return UpgradeResult::kCopyFailed;
                    }
                    ++stats.files_copied;
                } else {
                    ++stats.errors;
                    if (verbose_out)
                        *verbose_out << "ERROR copying " << rel << "\n";
                    return UpgradeResult::kCopyFailed;
                }
            } else {
                switch (opts.mode) {
                    case UpgradeMode::kCopy:
                        ++stats.files_copied;
                        break;
                    case UpgradeMode::kLink:
                        ++stats.files_linked;
                        break;
                    case UpgradeMode::kClone:
                        ++stats.files_cloned;
                        break;
                }
            }
            stats.bytes_migrated += bytes;
            if (verbose_out)
                *verbose_out << "migrate " << rel << " (" << bytes << " bytes)\n";
        }
    }

    if (verbose_out)
        *verbose_out << "upgrade complete\n";
    return UpgradeResult::kOk;
}

}  // namespace pgcpp::tools
