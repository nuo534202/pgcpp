// pg_waldump_test.cpp — Unit tests for the pg_waldump tool.
//
// Covers:
//   - RmgrName / RmgrIdFromName round-trip.
//   - FormatLsn / ParseLsn round-trip and rejection of bad input.
//   - DumpWal on the in-memory WAL buffer:
//       * empty buffer produces no output.
//       * a single XLogRecord is dumped with correct rmgr name, lsn, tx.
//       * rmgr filter only emits matching records.
//       * limit caps the number of records printed.
//       * stats mode accumulates per-rmgr counts and total_len.
//   - DumpWal from a file path (writes a small WAL file and reads it back).
//   - DumpWal error paths (missing file, invalid rmgr filter).
#include "tools/pg_waldump.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include "transaction/xlog.hpp"

using pgcpp::tools::DumpWal;
using pgcpp::tools::FormatLsn;
using pgcpp::tools::ParseLsn;
using pgcpp::tools::RmgrIdFromName;
using pgcpp::tools::RmgrName;
using pgcpp::tools::WaldumpOptions;
using pgcpp::tools::WaldumpResult;
using pgcpp::tools::WaldumpStats;
using pgcpp::transaction::kInvalidXLogRecPtr;
using pgcpp::transaction::kRmgrHeapId;
using pgcpp::transaction::kRmgrXactId;
using pgcpp::transaction::kRmgrXlogId;
using pgcpp::transaction::ResetWal;
using pgcpp::transaction::XLogRecord;
using pgcpp::transaction::XLogRecPtr;
using pgcpp::transaction::XLogWriteRaw;

namespace {

// Append an XLogRecord header (with `payload` bytes of zero data after it)
// to the in-memory WAL buffer and return the LSN of the new record.
XLogRecPtr AppendRecord(uint32_t tot_len, uint32_t xid, uint8_t rmid, uint8_t info, XLogRecPtr prev,
                        const std::vector<unsigned char>& payload) {
    XLogRecord rec;
    rec.xl_tot_len = tot_len;
    rec.xl_xid = xid;
    rec.xl_prev = prev;
    rec.xl_info = info;
    rec.xl_rmid = rmid;
    rec.xl_crc = 0;
    XLogRecPtr lsn = XLogWriteRaw(&rec, sizeof(rec));
    if (!payload.empty())
        XLogWriteRaw(payload.data(), payload.size());
    return lsn;
}

}  // namespace

// ===========================================================================
// RmgrName / RmgrIdFromName
// ===========================================================================

TEST(PgWaldumpTest, RmgrNameKnownIds) {
    EXPECT_STREQ(RmgrName(kRmgrXlogId), "XLOG");
    EXPECT_STREQ(RmgrName(kRmgrXactId), "Transaction");
    EXPECT_STREQ(RmgrName(kRmgrHeapId), "Heap");
}

TEST(PgWaldumpTest, RmgrNameUnknown) {
    EXPECT_STREQ(RmgrName(static_cast<pgcpp::transaction::RmgrId>(200)), "Unknown");
}

TEST(PgWaldumpTest, RmgrIdFromNameCaseInsensitive) {
    pgcpp::transaction::RmgrId id = 0;
    ASSERT_TRUE(RmgrIdFromName("XLOG", id));
    EXPECT_EQ(id, kRmgrXlogId);
    ASSERT_TRUE(RmgrIdFromName("xlog", id));
    EXPECT_EQ(id, kRmgrXlogId);
    ASSERT_TRUE(RmgrIdFromName("heap", id));
    EXPECT_EQ(id, kRmgrHeapId);
}

TEST(PgWaldumpTest, RmgrIdFromNameUnknownReturnsFalse) {
    pgcpp::transaction::RmgrId id = 0;
    EXPECT_FALSE(RmgrIdFromName("NoSuchRmgr", id));
    EXPECT_FALSE(RmgrIdFromName("", id));
}

// ===========================================================================
// FormatLsn / ParseLsn
// ===========================================================================

TEST(PgWaldumpTest, FormatLsnZero) {
    EXPECT_EQ(FormatLsn(kInvalidXLogRecPtr), "0/0");
}

TEST(PgWaldumpTest, FormatLsnRoundTrip) {
    const XLogRecPtr kSamples[] = {
        0x1,
        0x10,
        0x100,
        0x10000,
        0x100000000ULL,  // 1 in hi, 0 in lo
        0xABCDEF12ULL,
        0x12345678ABCDEF12ULL,
    };
    for (XLogRecPtr v : kSamples) {
        std::string s = FormatLsn(v);
        XLogRecPtr parsed = 0;
        ASSERT_TRUE(ParseLsn(s, parsed)) << "for LSN " << v;
        EXPECT_EQ(parsed, v) << "for LSN " << v << " (formatted: " << s << ")";
    }
}

TEST(PgWaldumpTest, ParseLsnBareHex) {
    XLogRecPtr v = 0;
    ASSERT_TRUE(ParseLsn("ff", v));
    EXPECT_EQ(v, 0xFFu);
}

TEST(PgWaldumpTest, ParseLsnRejectsEmpty) {
    XLogRecPtr v = 0;
    EXPECT_FALSE(ParseLsn("", v));
    EXPECT_FALSE(ParseLsn("/", v));
}

TEST(PgWaldumpTest, ParseLsnRejectsNonHex) {
    XLogRecPtr v = 0;
    EXPECT_FALSE(ParseLsn("xy", v));
    EXPECT_FALSE(ParseLsn("0/zz", v));
}

// ===========================================================================
// DumpWal on the in-memory WAL buffer
// ===========================================================================

TEST(PgWaldumpTest, EmptyBufferProducesNoOutput) {
    ResetWal();
    std::ostringstream out;
    WaldumpOptions opts;
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOk);
    EXPECT_TRUE(out.str().empty());
}

TEST(PgWaldumpTest, SingleRecordPrintsRmgrAndLsn) {
    ResetWal();
    // tot_len = header only (24 bytes), xid=42, rmid=XLOG, info=0.
    AppendRecord(sizeof(XLogRecord), /*xid=*/42, kRmgrXlogId, /*info=*/0,
                 /*prev=*/0, /*payload=*/{});
    std::ostringstream out;
    WaldumpOptions opts;
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOk);
    std::string s = out.str();
    EXPECT_NE(s.find("XLOG"), std::string::npos);
    EXPECT_NE(s.find("tx: 42"), std::string::npos);
    EXPECT_NE(s.find("prev: 0/0"), std::string::npos);
}

TEST(PgWaldumpTest, MultipleRecordsWithPayload) {
    ResetWal();
    // Record 1: header only at LSN 0.
    XLogRecPtr lsn1 = AppendRecord(sizeof(XLogRecord), /*xid=*/1, kRmgrXlogId,
                                   /*info=*/0, /*prev=*/0, /*payload=*/{});
    // Record 2: header + 8 bytes payload at the next 8-byte-aligned LSN.
    // After record 1 (24 bytes), the next aligned offset is 24.
    std::vector<unsigned char> payload(8, 0xAA);
    XLogRecPtr lsn2 = AppendRecord(sizeof(XLogRecord) + 8, /*xid=*/2, kRmgrXactId, /*info=*/0x10,
                                   /*prev=*/lsn1, payload);
    (void)lsn2;
    std::ostringstream out;
    WaldumpOptions opts;
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOk);
    std::string s = out.str();
    // Both records should be present.
    EXPECT_NE(s.find("XLOG"), std::string::npos);
    EXPECT_NE(s.find("Transaction"), std::string::npos);
    EXPECT_NE(s.find("tx: 1"), std::string::npos);
    EXPECT_NE(s.find("tx: 2"), std::string::npos);
}

TEST(PgWaldumpTest, RmgrFilterExcludesOthers) {
    ResetWal();
    AppendRecord(sizeof(XLogRecord), 1, kRmgrXlogId, 0, 0, {});
    std::vector<unsigned char> payload(8, 0xAA);
    AppendRecord(sizeof(XLogRecord) + 8, 2, kRmgrXactId, 0x10,
                 /*prev=*/0, payload);
    std::ostringstream out;
    WaldumpOptions opts;
    opts.rmgr_filter = "Transaction";
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOk);
    std::string s = out.str();
    EXPECT_NE(s.find("Transaction"), std::string::npos);
    EXPECT_EQ(s.find("rmgr: XLOG"), std::string::npos);
}

TEST(PgWaldumpTest, InvalidRmgrFilterFails) {
    ResetWal();
    AppendRecord(sizeof(XLogRecord), 1, kRmgrXlogId, 0, 0, {});
    std::ostringstream out;
    WaldumpOptions opts;
    opts.rmgr_filter = "NoSuchRmgr";
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kInvalidArgument);
}

TEST(PgWaldumpTest, LimitCapsRecords) {
    ResetWal();
    for (int i = 0; i < 5; ++i) {
        AppendRecord(sizeof(XLogRecord), static_cast<uint32_t>(i), kRmgrXlogId, /*info=*/0,
                     /*prev=*/0, /*payload=*/{});
    }
    std::ostringstream out;
    WaldumpOptions opts;
    opts.limit = 2;
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOk);
    // Count the number of newlines (one per record).
    std::string s = out.str();
    int nl = 0;
    for (char c : s)
        if (c == '\n')
            ++nl;
    EXPECT_EQ(nl, 2);
}

TEST(PgWaldumpTest, StartLsnSkipsRecords) {
    ResetWal();
    // First record lives at LSN kSizeofXlogRecord (24), since the first 24
    // bytes of the WAL stream are a reserved page-header area.
    AppendRecord(sizeof(XLogRecord), 100, kRmgrXlogId, 0, 0, {});
    // The second record begins at the next 8-byte-aligned offset after the
    // first record's end (24 + 24 = 48, already 8-aligned).
    XLogRecPtr second_lsn = pgcpp::transaction::kSizeofXlogRecord * 2;
    AppendRecord(sizeof(XLogRecord), 200, kRmgrXlogId, 0, 0, {});
    std::ostringstream out;
    WaldumpOptions opts;
    opts.start_lsn = second_lsn;
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOk);
    std::string s = out.str();
    EXPECT_EQ(s.find("tx: 100"), std::string::npos);
    EXPECT_NE(s.find("tx: 200"), std::string::npos);
}

TEST(PgWaldumpTest, StatsModeAccumulatesPerRmgr) {
    ResetWal();
    AppendRecord(sizeof(XLogRecord), 1, kRmgrXlogId, 0, 0, {});
    std::vector<unsigned char> payload(8, 0xAA);
    AppendRecord(sizeof(XLogRecord) + 8, 2, kRmgrXactId, 0, 0, payload);
    AppendRecord(sizeof(XLogRecord), 3, kRmgrXlogId, 0, 0, {});

    std::vector<WaldumpStats> stats;
    std::ostringstream out;
    WaldumpOptions opts;
    opts.stats = true;
    EXPECT_EQ(DumpWal(opts, out, &stats), WaldumpResult::kOk);

    // Two distinct rmgrs should be accumulated.
    EXPECT_EQ(stats.size(), 2u);
    std::size_t xlog_count = 0, xact_count = 0;
    std::uint64_t xlog_len = 0, xact_len = 0;
    for (const auto& s : stats) {
        if (s.rmgr_name == "XLOG") {
            xlog_count = s.count;
            xlog_len = s.total_len;
        } else if (s.rmgr_name == "Transaction") {
            xact_count = s.count;
            xact_len = s.total_len;
        }
    }
    EXPECT_EQ(xlog_count, 2u);
    EXPECT_EQ(xact_count, 1u);
    EXPECT_EQ(xlog_len, 2u * sizeof(XLogRecord));
    EXPECT_EQ(xact_len, sizeof(XLogRecord) + 8);
    // The summary should be printed in the stats-mode output.
    EXPECT_NE(out.str().find("WAL statistics"), std::string::npos);
}

// ===========================================================================
// DumpWal from a file path
// ===========================================================================

TEST(PgWaldumpTest, FilePathReadsRecords) {
    ResetWal();
    AppendRecord(sizeof(XLogRecord), 7, kRmgrHeapId, 0, 0, {});

    // Write the in-memory WAL buffer to a temp file.
    std::string path = "/tmp/pgcpp_waldump_file_" + std::to_string(getpid());
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f);
        const auto& buf = pgcpp::transaction::GetWalBuffer();
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
        ASSERT_TRUE(f);
    }

    std::ostringstream out;
    WaldumpOptions opts;
    opts.path = path;
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOk);
    std::string s = out.str();
    EXPECT_NE(s.find("Heap"), std::string::npos);
    EXPECT_NE(s.find("tx: 7"), std::string::npos);

    std::remove(path.c_str());
}

TEST(PgWaldumpTest, MissingFilePathFailsOpen) {
    std::ostringstream out;
    WaldumpOptions opts;
    opts.path = "/tmp/pgcpp_waldump_nonexistent_" + std::to_string(getpid());
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOpenFailed);
}

// ===========================================================================
// Short-record handling
// ===========================================================================

TEST(PgWaldumpTest, TruncatedHeaderFailsRead) {
    ResetWal();
    // Write a single byte to the WAL — less than the 24-byte header.
    unsigned char one = 0;
    XLogWriteRaw(&one, 1);

    std::ostringstream out;
    WaldumpOptions opts;
    EXPECT_EQ(DumpWal(opts, out), WaldumpResult::kOk);  // nothing to read
    EXPECT_TRUE(out.str().empty());
}
