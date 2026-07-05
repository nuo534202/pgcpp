// xlog_segment_test.cpp — Unit tests for WAL segment file support (Step 2).
//
// Tests:
//   - XLogFileName segment naming (24 hex chars: TLI + LogId + SegNo)
//   - LSN <-> segment arithmetic (RecPtrToXLogSegNo, RecPtrToSegmentOffset,
//     XLogSegNoOffsetToRecPtr)
//   - XLogFileInit / InstallXLogFileSegment file creation
//   - XLogFileOpen / XLogFileCopy file operations
//   - WalSegmentWriter single-segment write
//   - WalSegmentWriter cross-segment write with auto-switching
//   - WalSegmentWriter cross-segment continuous read-back
//
// Tests use a small segment size (1024 bytes) to force segment switching
// without writing 16MB. The WalSegmentWriter accepts a custom segment_size
// for this purpose.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "transaction/xlog.hpp"

using pgcpp::transaction::InstallXLogFileSegment;
using pgcpp::transaction::kDefaultTimelineId;
using pgcpp::transaction::kWalSegmentSize;
using pgcpp::transaction::kXLogSegmentsPerXLogId;
using pgcpp::transaction::RecPtrToSegmentOffset;
using pgcpp::transaction::RecPtrToXLogSegNo;
using pgcpp::transaction::TimeLineId;
using pgcpp::transaction::WalSegmentWriter;
using pgcpp::transaction::XLogFileCopy;
using pgcpp::transaction::XLogFileInit;
using pgcpp::transaction::XLogFileName;
using pgcpp::transaction::XLogFileOpen;
using pgcpp::transaction::XLogRecPtr;
using pgcpp::transaction::XLogSegNo;
using pgcpp::transaction::XLogSegNoOffsetToRecPtr;

namespace {

// Helper: create a fresh temp directory for a test. Returns the path.
std::string MakeTempDir(const std::string& name) {
    std::string path = "/tmp/" + name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directory(path);
    return path;
}

// Helper: read `len` bytes from `path` at `offset` and compare to `expected`.
::testing::AssertionResult ReadEquals(const std::string& path, off_t offset, std::size_t len,
                                      const uint8_t* expected) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return ::testing::AssertionFailure() << "open(" << path << ") failed: " << strerror(errno);
    }
    std::vector<uint8_t> buf(len, 0);
    if (lseek(fd, offset, SEEK_SET) < 0) {
        close(fd);
        return ::testing::AssertionFailure() << "lseek failed";
    }
    std::size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf.data() + got, len - got);
        if (n <= 0) {
            close(fd);
            return ::testing::AssertionFailure() << "read got " << got << " of " << len << " bytes";
        }
        got += static_cast<std::size_t>(n);
    }
    close(fd);
    if (std::memcmp(buf.data(), expected, len) != 0) {
        return ::testing::AssertionFailure() << "content mismatch at offset " << offset;
    }
    return ::testing::AssertionSuccess();
}

// ===========================================================================
// Segment naming
// ===========================================================================

TEST(XLogSegmentTest, FileName_FormatIs24HexChars) {
    std::string name = XLogFileName(kDefaultTimelineId, 1);
    EXPECT_EQ(name.size(), 24u);
    EXPECT_EQ(name, "000000010000000000000001");
}

TEST(XLogSegmentTest, FileName_TliOneSegnoZero) {
    // segno 0 -> "000000010000000000000000"
    EXPECT_EQ(XLogFileName(1, 0), "000000010000000000000000");
}

TEST(XLogSegmentTest, FileName_HighSegnoSplitsLogId) {
    // segno = 256 -> logid 1, seg 0 (256 segments per XLogId).
    // Format: TLI=1 (00000001) + LogId=1 (00000001) + Seg=0 (00000000).
    EXPECT_EQ(XLogFileName(1, 256), "000000010000000100000000");
    // segno = 257 -> logid 1, seg 1.
    EXPECT_EQ(XLogFileName(1, 257), "000000010000000100000001");
}

TEST(XLogSegmentTest, FileName_DifferentTimeline) {
    EXPECT_EQ(XLogFileName(2, 1), "000000020000000000000001");
    EXPECT_EQ(XLogFileName(0xAB, 0xCD), "000000AB00000000000000CD");
}

TEST(XLogSegmentTest, SegmentsPerXLogId_Is256) {
    EXPECT_EQ(kXLogSegmentsPerXLogId, 256u);
}

// ===========================================================================
// LSN arithmetic
// ===========================================================================

TEST(XLogSegmentTest, RecPtrToSegNo_LSN0) {
    EXPECT_EQ(RecPtrToXLogSegNo(0), XLogSegNo{0});
}

TEST(XLogSegmentTest, RecPtrToSegNo_LSNOneSegmentMinusOne) {
    EXPECT_EQ(RecPtrToXLogSegNo(kWalSegmentSize - 1), XLogSegNo{0});
}

TEST(XLogSegmentTest, RecPtrToSegNo_ExactSegmentBoundary) {
    EXPECT_EQ(RecPtrToXLogSegNo(kWalSegmentSize), XLogSegNo{1});
}

TEST(XLogSegmentTest, RecPtrToSegNo_HighLSN) {
    // 3 segments + 1 byte
    XLogRecPtr lsn = 3 * kWalSegmentSize + 1;
    EXPECT_EQ(RecPtrToXLogSegNo(lsn), XLogSegNo{3});
}

TEST(XLogSegmentTest, RecPtrToSegmentOffset_LSN0) {
    EXPECT_EQ(RecPtrToSegmentOffset(0), 0u);
}

TEST(XLogSegmentTest, RecPtrToSegmentOffset_WithinSegment) {
    EXPECT_EQ(RecPtrToSegmentOffset(100), 100u);
    EXPECT_EQ(RecPtrToSegmentOffset(kWalSegmentSize - 1), kWalSegmentSize - 1);
}

TEST(XLogSegmentTest, RecPtrToSegmentOffset_AtBoundary) {
    EXPECT_EQ(RecPtrToSegmentOffset(kWalSegmentSize), 0u);
    EXPECT_EQ(RecPtrToSegmentOffset(kWalSegmentSize + 5), 5u);
}

TEST(XLogSegmentTest, SegNoOffsetToRecPtr_RoundTrip) {
    for (XLogSegNo segno : {XLogSegNo{0}, XLogSegNo{1}, XLogSegNo{7}}) {
        for (uint32_t off : {0u, 1u, 100u, static_cast<uint32_t>(kWalSegmentSize - 1)}) {
            XLogRecPtr lsn = XLogSegNoOffsetToRecPtr(segno, off);
            EXPECT_EQ(RecPtrToXLogSegNo(lsn), segno);
            EXPECT_EQ(RecPtrToSegmentOffset(lsn), off);
        }
    }
}

// ===========================================================================
// XLogFileInit / InstallXLogFileSegment
// ===========================================================================

TEST(XLogSegmentTest, FileInit_CreatesZeroedFile) {
    std::string dir = MakeTempDir("pgcpp_xlog_seg_init");
    std::string path = dir + "/test_seg";
    ASSERT_TRUE(XLogFileInit(path));

    struct stat st;
    ASSERT_EQ(stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_size, static_cast<off_t>(kWalSegmentSize));

    // Verify first 16 bytes are zero (ftruncate zero-fills).
    std::vector<uint8_t> zeros(16, 0x00);
    EXPECT_TRUE(ReadEquals(path, 0, 16, zeros.data()));
    // Verify last 16 bytes are zero.
    EXPECT_TRUE(ReadEquals(path, kWalSegmentSize - 16, 16, zeros.data()));

    std::filesystem::remove_all(dir);
}

TEST(XLogSegmentTest, FileInit_ExistingFileIsNoOp) {
    std::string dir = MakeTempDir("pgcpp_xlog_seg_init2");
    std::string path = dir + "/existing";
    // Pre-create a non-empty file.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    const char msg[] = "hello";
    ssize_t w = write(fd, msg, sizeof(msg));
    ASSERT_EQ(w, static_cast<ssize_t>(sizeof(msg)));
    close(fd);

    // XLogFileInit should succeed (file exists) and NOT truncate it.
    EXPECT_TRUE(XLogFileInit(path));

    struct stat st;
    ASSERT_EQ(stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_size, static_cast<off_t>(sizeof(msg)));

    std::filesystem::remove_all(dir);
}

TEST(XLogSegmentTest, InstallXLogFileSegment_CreatesWithStandardName) {
    std::string dir = MakeTempDir("pgcpp_xlog_seg_install");
    ASSERT_TRUE(InstallXLogFileSegment(dir, kDefaultTimelineId, 1));

    std::string expected_path = dir + "/000000010000000000000001";
    struct stat st;
    EXPECT_EQ(stat(expected_path.c_str(), &st), 0);
    EXPECT_EQ(st.st_size, static_cast<off_t>(kWalSegmentSize));

    // Calling again on an existing segment is a no-op (still success).
    EXPECT_TRUE(InstallXLogFileSegment(dir, kDefaultTimelineId, 1));

    std::filesystem::remove_all(dir);
}

// ===========================================================================
// XLogFileOpen / XLogFileCopy
// ===========================================================================

TEST(XLogSegmentTest, FileOpen_ReturnsFdForExistingSegment) {
    std::string dir = MakeTempDir("pgcpp_xlog_seg_open");
    ASSERT_TRUE(InstallXLogFileSegment(dir, kDefaultTimelineId, 0));

    int fd = XLogFileOpen(dir, kDefaultTimelineId, 0, O_WRONLY, 0600);
    EXPECT_GE(fd, 0);
    if (fd >= 0)
        close(fd);

    // Non-existent segment returns -1.
    int fd2 = XLogFileOpen(dir, kDefaultTimelineId, 99, O_RDONLY, 0600);
    EXPECT_EQ(fd2, -1);

    std::filesystem::remove_all(dir);
}

TEST(XLogSegmentTest, FileCopy_ReplicatesContent) {
    std::string dir = MakeTempDir("pgcpp_xlog_seg_copy");
    std::string src = dir + "/src";
    std::string dst = dir + "/dst";

    // Write known content to src.
    int fd = open(src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    const uint8_t data[] = {1, 2, 3, 4, 5};
    ASSERT_EQ(write(fd, data, sizeof(data)), static_cast<ssize_t>(sizeof(data)));
    close(fd);

    ASSERT_TRUE(XLogFileCopy(dst, src));

    EXPECT_TRUE(ReadEquals(dst, 0, sizeof(data), data));

    std::filesystem::remove_all(dir);
}

// ===========================================================================
// WalSegmentWriter
// ===========================================================================

// Use a small segment size to force switching without 16MB of data.
constexpr uint32_t kTestSegmentSize = 1024;

TEST(WalSegmentWriterTest, Write_SingleSegment) {
    std::string dir = MakeTempDir("pgcpp_wal_seg_write1");
    WalSegmentWriter writer(dir, kDefaultTimelineId, kTestSegmentSize);

    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    XLogRecPtr lsn = 0;
    std::size_t n = writer.Write(lsn, data, sizeof(data));
    EXPECT_EQ(n, sizeof(data));

    // Verify content was written to segment 0 at offset 0.
    std::string seg0 = dir + "/" + XLogFileName(kDefaultTimelineId, 0);
    EXPECT_TRUE(ReadEquals(seg0, 0, sizeof(data), data));

    std::filesystem::remove_all(dir);
}

TEST(WalSegmentWriterTest, Write_AutoSwitchAtBoundary) {
    std::string dir = MakeTempDir("pgcpp_wal_seg_write2");
    WalSegmentWriter writer(dir, kDefaultTimelineId, kTestSegmentSize);

    // Write 8 bytes starting 4 bytes before the segment boundary (offset 1020).
    // Bytes 1020..1023 go to seg 0, bytes 1024..1027 go to seg 1.
    XLogRecPtr lsn = 1020;
    const uint8_t data[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    std::size_t n = writer.Write(lsn, data, sizeof(data));
    EXPECT_EQ(n, sizeof(data));

    // After the write, seg 1 should be the current open segment.
    EXPECT_EQ(writer.GetCurrentSegNo(), XLogSegNo{1});

    // Verify tail of seg 0.
    std::string seg0 = dir + "/" + XLogFileName(kDefaultTimelineId, 0);
    EXPECT_TRUE(ReadEquals(seg0, 1020, 4, data));

    // Verify head of seg 1.
    std::string seg1 = dir + "/" + XLogFileName(kDefaultTimelineId, 1);
    EXPECT_TRUE(ReadEquals(seg1, 0, 4, data + 4));

    std::filesystem::remove_all(dir);
}

TEST(WalSegmentWriterTest, Write_CrossSegmentContinuousReadback) {
    std::string dir = MakeTempDir("pgcpp_wal_seg_readback");
    WalSegmentWriter writer(dir, kDefaultTimelineId, kTestSegmentSize);

    // Build a 3-segment payload (3072 bytes) with a recognizable pattern.
    constexpr std::size_t kPayloadLen = 3 * kTestSegmentSize;
    std::vector<uint8_t> payload(kPayloadLen);
    for (std::size_t i = 0; i < kPayloadLen; i++) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Write from LSN 0 across 3 segments.
    XLogRecPtr lsn = 0;
    std::size_t n = writer.Write(lsn, payload.data(), payload.size());
    EXPECT_EQ(n, kPayloadLen);
    EXPECT_EQ(writer.GetCurrentSegNo(), XLogSegNo{2});
    writer.Flush();
    writer.Close();

    // Read back across all 3 segments and verify continuity.
    for (XLogSegNo segno : {XLogSegNo{0}, XLogSegNo{1}, XLogSegNo{2}}) {
        std::string path = dir + "/" + XLogFileName(kDefaultTimelineId, segno);
        int fd = open(path.c_str(), O_RDONLY);
        ASSERT_GE(fd, 0) << "segno " << segno;
        std::vector<uint8_t> seg_data(kTestSegmentSize, 0);
        ssize_t got = read(fd, seg_data.data(), kTestSegmentSize);
        close(fd);
        ASSERT_EQ(got, static_cast<ssize_t>(kTestSegmentSize));

        // Verify the segment's content matches the corresponding slice of
        // the payload.
        std::size_t payload_off = static_cast<std::size_t>(segno) * kTestSegmentSize;
        EXPECT_EQ(std::memcmp(seg_data.data(), payload.data() + payload_off, kTestSegmentSize), 0)
            << "mismatch in segno " << segno;
    }

    std::filesystem::remove_all(dir);
}

TEST(WalSegmentWriterTest, Write_MultiChunkWithinSameSegment) {
    std::string dir = MakeTempDir("pgcpp_wal_seg_multichunk");
    WalSegmentWriter writer(dir, kDefaultTimelineId, kTestSegmentSize);

    // Write two small chunks to the same segment at different LSNs.
    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {4, 5, 6};
    writer.Write(0, a, sizeof(a));
    writer.Write(100, b, sizeof(b));

    // Verify both chunks landed in segment 0.
    std::string seg0 = dir + "/" + XLogFileName(kDefaultTimelineId, 0);
    EXPECT_TRUE(ReadEquals(seg0, 0, sizeof(a), a));
    EXPECT_TRUE(ReadEquals(seg0, 100, sizeof(b), b));
    EXPECT_EQ(writer.GetCurrentSegNo(), XLogSegNo{0});

    std::filesystem::remove_all(dir);
}

TEST(WalSegmentWriterTest, Destructor_ClosesFile) {
    std::string dir = MakeTempDir("pgcpp_wal_seg_dtor");
    {
        WalSegmentWriter writer(dir, kDefaultTimelineId, kTestSegmentSize);
        const uint8_t data = 0x42;
        writer.Write(0, &data, 1);
        // No explicit Close — destructor must close the fd.
    }
    // A new writer should be able to open the same segment (file not locked).
    WalSegmentWriter w2(dir, kDefaultTimelineId, kTestSegmentSize);
    const uint8_t data2 = 0x99;
    EXPECT_EQ(w2.Write(10, &data2, 1), 1u);

    std::filesystem::remove_all(dir);
}

}  // namespace
