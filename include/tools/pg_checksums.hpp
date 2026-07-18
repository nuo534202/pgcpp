// pg_checksums.h — Data page checksum verifier (pg_checksums).
//
// Converted from PostgreSQL 15's src/bin/pg_checksums/.
//
// pg_checksums scans all heap/index relation files in a data directory
// and verifies (or repairs) each page's 16-bit checksum. Pages whose
// pd_flags lacks the kPageChecksumValid bit are skipped (no checksum).
//
// pgcpp's checksum algorithm is a simplified FNV-1a 16-bit fold over the
// page contents (excluding the 2-byte pd_checksum field itself), mixed
// with the block number to make checksums position-dependent. This is
// distinct from PostgreSQL's parallel-lane algorithm in checksum_impl.h,
// but follows the same contract: any single-bit flip changes the result.
//
// Usage:
//   pg_checksums -D <data_dir> [--check] [--verbose]
//   pg_checksums -D <data_dir> --disable
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace pgcpp::tools {

// ChecksumsMode — what pg_checksums should do with each page.
enum class ChecksumsMode {
    kCheck,    // verify checksums (default)
    kEnable,   // compute and write checksums on every page
    kDisable,  // clear checksums (set pd_checksum=0, clear kPageChecksumValid)
};

// ChecksumsOptions — inputs to RunChecksums.
struct ChecksumsOptions {
    // Path to the data directory (PGDATA). The scan walks <data_dir>/base/.
    std::string data_dir;

    // Operation mode.
    ChecksumsMode mode = ChecksumsMode::kCheck;

    // If true, print one line per scanned file.
    bool verbose = false;
};

// ChecksumsResult — outcome of a checksum run.
enum class ChecksumsResult {
    kOk,                // all pages verified (or successfully repaired)
    kOpenFailed,        // could not open the data_dir or a relation file
    kReadFailed,        // short read or unexpected EOF on a relation file
    kChecksumMismatch,  // at least one page's checksum did not match
    kInvalidArgument,   // empty data_dir or bad mode
};

// ChecksumsStats — aggregate counters accumulated by RunChecksums.
struct ChecksumsStats {
    std::size_t files_scanned = 0;  // number of relation files visited
    std::size_t pages_scanned = 0;  // total pages read across all files
    std::size_t bad_checksums = 0;  // pages whose checksum did not match
    std::size_t skipped = 0;        // pages without checksums (no kPageChecksumValid)
};

// PageChecksum — compute the 16-bit checksum of a single page.
//
// `page` is the raw page contents (must be `page_size` bytes). The 2-byte
// pd_checksum field at offset 8..9 is excluded from the computation. The
// `blkno` parameter is mixed in at the end to make checksums
// position-dependent (a page's checksum changes when it is moved to a
// different block).
//
// Returns the 16-bit checksum. The algorithm is a simplified FNV-1a:
//   acc = FNV-1a-32 over page bytes (excluding bytes 8..9)
//   acc ^= blkno
//   result = (acc ^ (acc >> 16)) & 0xFFFF
std::uint16_t PageChecksum(const char* page, std::size_t page_size, std::uint32_t blkno);

// VerifyPageChecksum — check that the page's stored checksum matches the
// recomputed value. Returns true if the page has no checksum (no
// kPageChecksumValid flag) or if the stored checksum matches the
// recomputed one. Returns false if the stored checksum is non-zero but
// does not match.
bool VerifyPageChecksum(const char* page, std::size_t page_size, std::uint32_t blkno);

// SetPageChecksum — compute the checksum and store it in the page's
// pd_checksum field, setting the kPageChecksumValid flag. Returns the
// new checksum. The page is modified in place.
std::uint16_t SetPageChecksum(char* page, std::size_t page_size, std::uint32_t blkno);

// ClearPageChecksum — set pd_checksum to 0 and clear the kPageChecksumValid
// flag. The page is modified in place.
void ClearPageChecksum(char* page, std::size_t page_size);

// RunChecksums — scan all relation files under <data_dir>/base/ and
// verify (or repair) page checksums. Returns the aggregate result and
// accumulates per-file/per-page counters in `stats`.
ChecksumsResult RunChecksums(const ChecksumsOptions& opts, std::ostream& out,
                             ChecksumsStats* stats = nullptr);

}  // namespace pgcpp::tools
