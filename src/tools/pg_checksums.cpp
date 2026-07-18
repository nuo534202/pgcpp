// pg_checksums.cpp — Data page checksum verifier (pg_checksums).
//
// Implements a simplified FNV-1a page checksum and a directory scanner
// that walks <data_dir>/base/<dboid>/<relfilenode> files. For each file,
// reads page-aligned blocks and verifies (or repairs) the checksum.
#include "tools/pg_checksums.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <ostream>
#include <string>
#include <vector>

#include "storage/block.hpp"    // kBlckSz
#include "storage/bufpage.hpp"  // PageHeaderData, kPageChecksumValid

namespace pgcpp::tools {

namespace {

// FNV-1a parameters (32-bit).
constexpr std::uint32_t kFnvOffsetBasis = 0x811c9dc5u;
constexpr std::uint32_t kFnvPrime = 0x01000193u;

// Offset of pd_checksum within PageHeaderData (8 bytes: pd_lsn).
constexpr std::size_t kChecksumOffset = 8;

// IsRelationFile — heuristic to identify a relation file under base/<db>/.
// Relation files have a numeric (decimal) base name, optionally followed
// by a fork suffix (e.g. "12345_fsm", "12345_vm", "12345_init").
bool IsRelationFile(const std::string& name) {
    if (name.empty() || name[0] < '0' || name[0] > '9')
        return false;
    // Scan a leading decimal run.
    std::size_t i = 0;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9')
        ++i;
    if (i == 0)
        return false;
    // Either end of string, or a fork suffix.
    if (i == name.size())
        return true;
    // Accept the standard fork suffixes.
    static const char* kSuffixes[] = {"_fsm", "_vm", "_init"};
    for (const char* sfx : kSuffixes) {
        std::size_t len = std::strlen(sfx);
        if (name.size() - i == len && name.compare(i, len, sfx) == 0)
            return true;
    }
    return false;
}

// ListSubdirs — return sorted list of subdirectory names under `path`.
std::vector<std::string> ListSubdirs(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (d == nullptr)
        return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..")
            continue;
        std::string full = path + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// ListRelationFiles — return sorted list of relation file names under `dir`.
std::vector<std::string> ListRelationFiles(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr)
        return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..")
            continue;
        if (!IsRelationFile(name))
            continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue;
        out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// Fnva1Update — fold `len` bytes of `data` into the FNV-1a accumulator.
// The range `[skip_off, skip_off + skip_len)` in `data` is excluded from
// the computation (used to skip the 2-byte pd_checksum field).
std::uint32_t Fnva1Update(std::uint32_t acc, const char* data, std::size_t len,
                          std::size_t skip_off, std::size_t skip_len) {
    for (std::size_t i = 0; i < len; ++i) {
        if (i >= skip_off && i < skip_off + skip_len)
            continue;
        acc ^= static_cast<std::uint8_t>(data[i]);
        acc *= kFnvPrime;
    }
    return acc;
}

}  // namespace

std::uint16_t PageChecksum(const char* page, std::size_t page_size, std::uint32_t blkno) {
    std::uint32_t acc = kFnvOffsetBasis;
    // Exclude the 2-byte pd_checksum field from the hash.
    acc = Fnva1Update(acc, page, page_size, kChecksumOffset, sizeof(std::uint16_t));
    // Mix in the block number to make checksums position-dependent.
    acc ^= static_cast<std::uint32_t>(blkno);
    // Fold to 16 bits.
    return static_cast<std::uint16_t>((acc ^ (acc >> 16)) & 0xFFFFu);
}

bool VerifyPageChecksum(const char* page, std::size_t page_size, std::uint32_t blkno) {
    auto* phdr = reinterpret_cast<const pgcpp::storage::PageHeaderData*>(page);
    // If the page has no checksum (kPageChecksumValid not set), accept it.
    if ((phdr->pd_flags & pgcpp::storage::kPageChecksumValid) == 0)
        return true;
    std::uint16_t stored = phdr->pd_checksum;
    std::uint16_t computed = PageChecksum(page, page_size, blkno);
    return stored == computed;
}

std::uint16_t SetPageChecksum(char* page, std::size_t page_size, std::uint32_t blkno) {
    auto* phdr = reinterpret_cast<pgcpp::storage::PageHeaderData*>(page);
    // Set the kPageChecksumValid flag.
    phdr->pd_flags |= pgcpp::storage::kPageChecksumValid;
    // Compute the checksum (which reads pd_checksum as whatever it currently
    // is — that's why the field is excluded from the hash).
    std::uint16_t c = PageChecksum(page, page_size, blkno);
    phdr->pd_checksum = c;
    return c;
}

void ClearPageChecksum(char* page, std::size_t page_size) {
    (void)page_size;
    auto* phdr = reinterpret_cast<pgcpp::storage::PageHeaderData*>(page);
    phdr->pd_flags &= ~pgcpp::storage::kPageChecksumValid;
    phdr->pd_checksum = 0;
}

ChecksumsResult RunChecksums(const ChecksumsOptions& opts, std::ostream& out,
                             ChecksumsStats* stats) {
    if (opts.data_dir.empty())
        return ChecksumsResult::kInvalidArgument;

    std::string base_dir = opts.data_dir + "/base";
    auto db_dirs = ListSubdirs(base_dir);
    if (db_dirs.empty()) {
        // No base/<db>/ directories — either the dir doesn't exist or there's
        // nothing to scan. Treat "directory missing" as kOpenFailed.
        struct stat st;
        if (stat(base_dir.c_str(), &st) != 0)
            return ChecksumsResult::kOpenFailed;
        // Directory exists but is empty — that's a successful no-op scan.
        return ChecksumsResult::kOk;
    }

    bool any_mismatch = false;
    const int page_size = pgcpp::storage::kBlckSz;

    for (const auto& db_name : db_dirs) {
        std::string db_dir = base_dir + "/" + db_name;
        auto rels = ListRelationFiles(db_dir);
        for (const auto& rel_name : rels) {
            std::string rel_path = db_dir + "/" + rel_name;

            // Open with in|out (no truncation) for kEnable/kDisable so we can
            // seekp to the modified block and write it back. For kCheck we
            // only need read access.
            std::ios::openmode mode = std::ios::binary;
            if (opts.mode == ChecksumsMode::kCheck) {
                mode |= std::ios::in;
            } else {
                mode |= std::ios::in | std::ios::out;
            }
            std::fstream f(rel_path, mode);
            if (!f) {
                out << "cannot open: " << rel_path << "\n";
                return ChecksumsResult::kOpenFailed;
            }

            // Determine the file size to know the block count.
            f.seekg(0, std::ios::end);
            auto end_pos = f.tellg();
            if (end_pos < 0) {
                out << "cannot stat: " << rel_path << "\n";
                return ChecksumsResult::kReadFailed;
            }
            std::size_t file_size = static_cast<std::size_t>(end_pos);
            if (file_size % page_size != 0) {
                // Truncated trailing bytes — skip the partial block.
                file_size -= file_size % page_size;
            }
            std::size_t nblocks = file_size / page_size;

            if (opts.verbose)
                out << "scanning: " << rel_path << " (" << nblocks << " blocks)\n";

            f.seekg(0, std::ios::beg);
            std::vector<char> page(page_size);
            for (std::size_t blkno = 0; blkno < nblocks; ++blkno) {
                auto byte_off = static_cast<std::streamoff>(blkno * page_size);
                f.seekg(byte_off, std::ios::beg);
                f.read(page.data(), page_size);
                if (static_cast<std::size_t>(f.gcount()) != page_size) {
                    out << "short read on block " << blkno << " of " << rel_path << "\n";
                    return ChecksumsResult::kReadFailed;
                }
                if (stats != nullptr) {
                    stats->pages_scanned += 1;
                }

                auto* phdr = reinterpret_cast<pgcpp::storage::PageHeaderData*>(page.data());
                bool has_checksum = (phdr->pd_flags & pgcpp::storage::kPageChecksumValid) != 0;

                if (opts.mode == ChecksumsMode::kCheck) {
                    if (!has_checksum) {
                        if (stats != nullptr)
                            stats->skipped += 1;
                        continue;
                    }
                    if (!VerifyPageChecksum(page.data(), page.size(),
                                            static_cast<std::uint32_t>(blkno))) {
                        any_mismatch = true;
                        if (stats != nullptr)
                            stats->bad_checksums += 1;
                        out << "bad checksum: " << rel_path << " block " << blkno << "\n";
                    }
                } else if (opts.mode == ChecksumsMode::kEnable) {
                    SetPageChecksum(page.data(), page.size(), static_cast<std::uint32_t>(blkno));
                    f.seekp(byte_off, std::ios::beg);
                    f.write(page.data(), page_size);
                    if (!f) {
                        out << "short write on block " << blkno << " of " << rel_path << "\n";
                        return ChecksumsResult::kReadFailed;
                    }
                } else if (opts.mode == ChecksumsMode::kDisable) {
                    ClearPageChecksum(page.data(), page.size());
                    f.seekp(byte_off, std::ios::beg);
                    f.write(page.data(), page_size);
                    if (!f) {
                        out << "short write on block " << blkno << " of " << rel_path << "\n";
                        return ChecksumsResult::kReadFailed;
                    }
                }
            }

            if (stats != nullptr)
                stats->files_scanned += 1;
        }
    }

    if (opts.mode == ChecksumsMode::kCheck && any_mismatch)
        return ChecksumsResult::kChecksumMismatch;
    return ChecksumsResult::kOk;
}

}  // namespace pgcpp::tools
