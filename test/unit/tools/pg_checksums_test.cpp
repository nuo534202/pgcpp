// pg_checksums_test.cpp — Unit tests for the pg_checksums tool.
//
// Covers:
//   - PageChecksum determinism (same content + blkno -> same hash).
//   - PageChecksum is position-dependent (different blkno -> different hash).
//   - PageChecksum excludes the pd_checksum field (modifying it does not
//     change the computed hash).
//   - PageChecksum is sensitive to a single-bit flip in page content.
//   - SetPageChecksum stores a verifiable checksum and sets the
//     kPageChecksumValid flag.
//   - VerifyPageChecksum accepts pages without the flag (no checksum).
//   - VerifyPageChecksum rejects corrupted pages (single byte flipped).
//   - ClearPageChecksum clears the flag and zeroes the field.
//   - RunChecksums scans a fake data directory:
//       * empty data dir -> kOpenFailed.
//       * empty base dir -> kOk.
//       * one file with a valid checksum -> kOk.
//       * one file with a corrupted page -> kChecksumMismatch.
//       * enable mode writes checksums; subsequent check passes.
//       * disable mode clears checksums; subsequent check skips all pages.
#include "tools/pg_checksums.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include "storage/block.hpp"
#include "storage/bufpage.hpp"

using pgcpp::storage::kBlckSz;
using pgcpp::storage::kPageChecksumValid;
using pgcpp::storage::PageHeaderData;
using pgcpp::tools::ChecksumsMode;
using pgcpp::tools::ChecksumsOptions;
using pgcpp::tools::ChecksumsResult;
using pgcpp::tools::ChecksumsStats;
using pgcpp::tools::ClearPageChecksum;
using pgcpp::tools::PageChecksum;
using pgcpp::tools::RunChecksums;
using pgcpp::tools::SetPageChecksum;
using pgcpp::tools::VerifyPageChecksum;

namespace {

// BuildPage — create an 8KB page with a known header and some content.
// `content_seed` controls the byte pattern written into the page body
// (after the 24-byte header). The pd_checksum field is left zero.
std::vector<char> BuildPage(unsigned char content_seed) {
    std::vector<char> page(kBlckSz, 0);
    auto* phdr = reinterpret_cast<PageHeaderData*>(page.data());
    phdr->pd_lsn = 0;
    phdr->pd_checksum = 0;
    phdr->pd_flags = 0;
    phdr->pd_lower = 24;  // header only
    phdr->pd_upper = kBlckSz;
    phdr->pd_special = kBlckSz;
    phdr->pd_pagesize_version = static_cast<uint16_t>(kBlckSz) | 4;
    phdr->pd_prune_xid = 0;
    for (int i = 24; i < kBlckSz; ++i)
        page[i] = static_cast<char>(content_seed + i);
    return page;
}

// MakeTempDataDir — create /tmp/<prefix>_<pid>/ and return its path.
std::string MakeTempDataDir(const std::string& prefix) {
    std::string dir = "/tmp/" + prefix + "_" + std::to_string(getpid());
    std::string rm = "rm -rf " + dir;
    EXPECT_EQ(system(rm.c_str()), 0);
    EXPECT_EQ(mkdir(dir.c_str(), 0700), 0);
    return dir;
}

// WriteRelationPage — write `pages` (each kBlckSz bytes) to a single
// relation file at <data_dir>/base/<db>/<relfilenode>. Creates parent
// directories (base/, base/<db>/) as needed.
void WriteRelationFile(const std::string& data_dir, const std::string& db_name,
                       const std::string& rel_name, const std::vector<std::vector<char>>& pages) {
    std::string base_dir = data_dir + "/base";
    EXPECT_EQ(mkdir(base_dir.c_str(), 0700), 0);
    std::string db_dir = base_dir + "/" + db_name;
    EXPECT_EQ(mkdir(db_dir.c_str(), 0700), 0);
    std::string path = db_dir + "/" + rel_name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(f);
    for (const auto& p : pages) {
        ASSERT_EQ(p.size(), static_cast<std::size_t>(kBlckSz));
        f.write(p.data(), kBlckSz);
        ASSERT_TRUE(f);
    }
}

}  // namespace

// ===========================================================================
// PageChecksum properties
// ===========================================================================

TEST(PgChecksumsTest, PageChecksumDeterministic) {
    auto p = BuildPage(/*seed=*/1);
    auto c1 = PageChecksum(p.data(), p.size(), /*blkno=*/0);
    auto c2 = PageChecksum(p.data(), p.size(), /*blkno=*/0);
    EXPECT_EQ(c1, c2);
}

TEST(PgChecksumsTest, PageChecksumPositionDependent) {
    auto p = BuildPage(/*seed=*/1);
    auto c1 = PageChecksum(p.data(), p.size(), /*blkno=*/0);
    auto c2 = PageChecksum(p.data(), p.size(), /*blkno=*/1);
    EXPECT_NE(c1, c2);
}

TEST(PgChecksumsTest, PageChecksumExcludesChecksumField) {
    auto p = BuildPage(/*seed=*/1);
    auto c1 = PageChecksum(p.data(), p.size(), /*blkno=*/0);
    // Modify the pd_checksum field (bytes 8..9) — the computed checksum
    // must not change because the field is excluded from the hash.
    auto* phdr = reinterpret_cast<PageHeaderData*>(p.data());
    phdr->pd_checksum = 0xFFFFu;
    auto c2 = PageChecksum(p.data(), p.size(), /*blkno=*/0);
    EXPECT_EQ(c1, c2);
}

TEST(PgChecksumsTest, PageChecksumBitFlipChangesHash) {
    auto p = BuildPage(/*seed=*/1);
    auto c1 = PageChecksum(p.data(), p.size(), /*blkno=*/0);
    // Flip one byte of the page body (after the header).
    p[100] ^= 0x01;
    auto c2 = PageChecksum(p.data(), p.size(), /*blkno=*/0);
    EXPECT_NE(c1, c2);
}

TEST(PgChecksumsTest, PageChecksumDifferentContentDifferentHash) {
    auto p1 = BuildPage(/*seed=*/1);
    auto p2 = BuildPage(/*seed=*/2);
    auto c1 = PageChecksum(p1.data(), p1.size(), /*blkno=*/0);
    auto c2 = PageChecksum(p2.data(), p2.size(), /*blkno=*/0);
    EXPECT_NE(c1, c2);
}

// ===========================================================================
// SetPageChecksum / VerifyPageChecksum / ClearPageChecksum
// ===========================================================================

TEST(PgChecksumsTest, SetPageChecksumSetsValidFlagAndMatchesVerify) {
    auto p = BuildPage(/*seed=*/5);
    uint16_t c = SetPageChecksum(p.data(), p.size(), /*blkno=*/0);
    auto* phdr = reinterpret_cast<PageHeaderData*>(p.data());
    EXPECT_NE(phdr->pd_checksum, 0);
    EXPECT_EQ(phdr->pd_checksum, c);
    EXPECT_TRUE((phdr->pd_flags & kPageChecksumValid) != 0);
    EXPECT_TRUE(VerifyPageChecksum(p.data(), p.size(), /*blkno=*/0));
}

TEST(PgChecksumsTest, VerifyAcceptsPageWithoutChecksumFlag) {
    auto p = BuildPage(/*seed=*/5);
    // No kPageChecksumValid flag set — VerifyPageChecksum should accept it
    // regardless of the pd_checksum value.
    EXPECT_TRUE(VerifyPageChecksum(p.data(), p.size(), /*blkno=*/0));
}

TEST(PgChecksumsTest, VerifyRejectsCorruptedPage) {
    auto p = BuildPage(/*seed=*/5);
    SetPageChecksum(p.data(), p.size(), /*blkno=*/0);
    EXPECT_TRUE(VerifyPageChecksum(p.data(), p.size(), /*blkno=*/0));
    // Flip a byte in the page body — the checksum no longer matches.
    p[100] ^= 0x01;
    EXPECT_FALSE(VerifyPageChecksum(p.data(), p.size(), /*blkno=*/0));
}

TEST(PgChecksumsTest, VerifyRejectsWrongBlkno) {
    auto p = BuildPage(/*seed=*/5);
    SetPageChecksum(p.data(), p.size(), /*blkno=*/0);
    // The same page at a different block number should not verify.
    EXPECT_FALSE(VerifyPageChecksum(p.data(), p.size(), /*blkno=*/1));
}

TEST(PgChecksumsTest, ClearPageChecksumZeroesField) {
    auto p = BuildPage(/*seed=*/5);
    SetPageChecksum(p.data(), p.size(), /*blkno=*/0);
    auto* phdr = reinterpret_cast<PageHeaderData*>(p.data());
    EXPECT_NE(phdr->pd_checksum, 0);
    EXPECT_TRUE((phdr->pd_flags & kPageChecksumValid) != 0);
    ClearPageChecksum(p.data(), p.size());
    EXPECT_EQ(phdr->pd_checksum, 0);
    EXPECT_FALSE((phdr->pd_flags & kPageChecksumValid) != 0);
}

// ===========================================================================
// RunChecksums integration tests
// ===========================================================================

TEST(PgChecksumsTest, EmptyDataDirFailsOpen) {
    std::string dir = "/tmp/pgcpp_checksums_empty_" + std::to_string(getpid());
    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
    EXPECT_EQ(mkdir(dir.c_str(), 0700), 0);  // no base/ subdir
    ChecksumsOptions opts;
    opts.data_dir = dir;
    ChecksumsStats stats;
    std::ostringstream out;
    EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOpenFailed);
    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgChecksumsTest, EmptyBaseDirReturnsOk) {
    std::string dir = "/tmp/pgcpp_checksums_emptybase_" + std::to_string(getpid());
    EXPECT_EQ(system(("rm -rf " + dir + " && mkdir -p " + dir + "/base").c_str()), 0);
    ChecksumsOptions opts;
    opts.data_dir = dir;
    ChecksumsStats stats;
    std::ostringstream out;
    EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
    EXPECT_EQ(stats.files_scanned, 0u);
    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgChecksumsTest, InvalidArgEmptyDataDir) {
    ChecksumsOptions opts;
    opts.data_dir = "";
    std::ostringstream out;
    EXPECT_EQ(RunChecksums(opts, out), ChecksumsResult::kInvalidArgument);
}

TEST(PgChecksumsTest, CheckOnValidPagePasses) {
    std::string dir = MakeTempDataDir("pgcpp_checksums_valid");
    auto p = BuildPage(/*seed=*/7);
    SetPageChecksum(p.data(), p.size(), /*blkno=*/0);
    WriteRelationFile(dir, "16384", "16385", {p});

    ChecksumsOptions opts;
    opts.data_dir = dir;
    opts.mode = ChecksumsMode::kCheck;
    ChecksumsStats stats;
    std::ostringstream out;
    EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
    EXPECT_EQ(stats.files_scanned, 1u);
    EXPECT_EQ(stats.pages_scanned, 1u);
    EXPECT_EQ(stats.bad_checksums, 0u);
    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgChecksumsTest, CheckOnCorruptedPageFails) {
    std::string dir = MakeTempDataDir("pgcpp_checksums_corrupt");
    auto p = BuildPage(/*seed=*/7);
    SetPageChecksum(p.data(), p.size(), /*blkno=*/0);
    // Flip a byte in the page body.
    p[100] ^= 0x01;
    WriteRelationFile(dir, "16384", "16385", {p});

    ChecksumsOptions opts;
    opts.data_dir = dir;
    opts.mode = ChecksumsMode::kCheck;
    ChecksumsStats stats;
    std::ostringstream out;
    EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kChecksumMismatch);
    EXPECT_EQ(stats.bad_checksums, 1u);
    EXPECT_NE(out.str().find("bad checksum"), std::string::npos);
    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgChecksumsTest, CheckSkipsPagesWithoutChecksumFlag) {
    std::string dir = MakeTempDataDir("pgcpp_checksums_skip");
    auto p = BuildPage(/*seed=*/7);
    // Do NOT call SetPageChecksum — the page has no kPageChecksumValid flag.
    WriteRelationFile(dir, "16384", "16385", {p});

    ChecksumsOptions opts;
    opts.data_dir = dir;
    opts.mode = ChecksumsMode::kCheck;
    ChecksumsStats stats;
    std::ostringstream out;
    EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
    EXPECT_EQ(stats.skipped, 1u);
    EXPECT_EQ(stats.bad_checksums, 0u);
    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgChecksumsTest, EnableThenCheckPasses) {
    std::string dir = MakeTempDataDir("pgcpp_checksums_enable");
    auto p1 = BuildPage(/*seed=*/1);
    auto p2 = BuildPage(/*seed=*/2);
    WriteRelationFile(dir, "16384", "16385", {p1, p2});

    // Enable checksums on both pages.
    {
        ChecksumsOptions opts;
        opts.data_dir = dir;
        opts.mode = ChecksumsMode::kEnable;
        ChecksumsStats stats;
        std::ostringstream out;
        EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
        EXPECT_EQ(stats.files_scanned, 1u);
        EXPECT_EQ(stats.pages_scanned, 2u);
    }

    // Now check — should pass.
    {
        ChecksumsOptions opts;
        opts.data_dir = dir;
        opts.mode = ChecksumsMode::kCheck;
        ChecksumsStats stats;
        std::ostringstream out;
        EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
        EXPECT_EQ(stats.bad_checksums, 0u);
        EXPECT_EQ(stats.skipped, 0u);
    }

    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgChecksumsTest, DisableThenCheckSkipsAll) {
    std::string dir = MakeTempDataDir("pgcpp_checksums_disable");
    auto p = BuildPage(/*seed=*/3);
    SetPageChecksum(p.data(), p.size(), /*blkno=*/0);
    WriteRelationFile(dir, "16384", "16385", {p});

    // Disable checksums — should clear the flag.
    {
        ChecksumsOptions opts;
        opts.data_dir = dir;
        opts.mode = ChecksumsMode::kDisable;
        ChecksumsStats stats;
        std::ostringstream out;
        EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
        EXPECT_EQ(stats.files_scanned, 1u);
    }

    // Now check — page should be skipped (no flag set).
    {
        ChecksumsOptions opts;
        opts.data_dir = dir;
        opts.mode = ChecksumsMode::kCheck;
        ChecksumsStats stats;
        std::ostringstream out;
        EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
        EXPECT_EQ(stats.skipped, 1u);
        EXPECT_EQ(stats.bad_checksums, 0u);
    }

    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgChecksumsTest, VerboseOutputsFileName) {
    std::string dir = MakeTempDataDir("pgcpp_checksums_verbose");
    auto p = BuildPage(/*seed=*/4);
    SetPageChecksum(p.data(), p.size(), /*blkno=*/0);
    WriteRelationFile(dir, "16384", "16385", {p});

    ChecksumsOptions opts;
    opts.data_dir = dir;
    opts.mode = ChecksumsMode::kCheck;
    opts.verbose = true;
    ChecksumsStats stats;
    std::ostringstream out;
    EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
    EXPECT_NE(out.str().find("scanning:"), std::string::npos);

    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgChecksumsTest, MultipleBlocksPerFile) {
    std::string dir = MakeTempDataDir("pgcpp_checksums_multiblock");
    std::vector<std::vector<char>> pages;
    for (int blk = 0; blk < 3; ++blk) {
        auto p = BuildPage(static_cast<unsigned char>(blk + 1));
        SetPageChecksum(p.data(), p.size(), static_cast<uint32_t>(blk));
        pages.push_back(std::move(p));
    }
    WriteRelationFile(dir, "16384", "16385", pages);

    ChecksumsOptions opts;
    opts.data_dir = dir;
    opts.mode = ChecksumsMode::kCheck;
    ChecksumsStats stats;
    std::ostringstream out;
    EXPECT_EQ(RunChecksums(opts, out, &stats), ChecksumsResult::kOk);
    EXPECT_EQ(stats.pages_scanned, 3u);
    EXPECT_EQ(stats.bad_checksums, 0u);

    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}
