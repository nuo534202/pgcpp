// xlog_fpw_test.cpp — Unit tests for full-page image (FPW) backup blocks (Step 4).
//
// Tests:
//   - FPW flag set in xl_info when a registered buffer has is_fpw=true
//   - FPW flag NOT set when no buffer has is_fpw=true
//   - Backup block content (block_id + page_data) round-trips correctly
//   - Multiple backup blocks in one record
//   - Main data is correctly separated from backup blocks
//   - Non-FPW buffer registration does not produce backup blocks
//   - Re-registering the same block_id replaces the previous entry
//   - Empty page data is ignored (no backup block emitted)
//   - Recovery passes correct main_data to redo callback for FPW records

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "transaction/xlog.hpp"
#include "transaction/xloginsert.hpp"
#include "transaction/xlogreader.hpp"
#include "transaction/xlogrecovery.hpp"

using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::InitializeWal;
using pgcpp::transaction::kRmgrHeapId;
using pgcpp::transaction::kXlrBkpBlockImage;
using pgcpp::transaction::PerformCrashRecovery;
using pgcpp::transaction::RecoveryStats;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::ResetWal;
using pgcpp::transaction::ResetXlogInsertState;
using pgcpp::transaction::SetWalDirectory;
using pgcpp::transaction::XLogBeginInsert;
using pgcpp::transaction::XLogInsert;
using pgcpp::transaction::XLogReaderAlloc;
using pgcpp::transaction::XLogReaderFree;
using pgcpp::transaction::XLogReadRecordAt;
using pgcpp::transaction::XLogRecord;
using pgcpp::transaction::XLogRecPtr;
using pgcpp::transaction::XLogRegisterBuffer;
using pgcpp::transaction::XLogRegisterData;

namespace {

class XLogFpwTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("xlog_fpw_test");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        ResetTransactionState();
        InitializeTransactionSystem();
        SetWalDirectory("");
        InitializeWal();
    }

    void TearDown() override {
        ResetTransactionState();
        ResetWal();
        SetWalDirectory("");
        ResetXlogInsertState();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    pgcpp::memory::MemoryContext* context_ = nullptr;
};

// --------------------------------------------------------------------------
// FPW flag and backup block presence
// --------------------------------------------------------------------------

// Registering a buffer with is_fpw=true sets kXlrBkpBlockImage in xl_info.
TEST_F(XLogFpwTest, FpwFlagSetWhenBufferIsFpw) {
    std::vector<uint8_t> page(8, 0xAB);
    XLogBeginInsert();
    XLogRegisterBuffer(0, page.data(), page.size(), /*is_fpw=*/true);
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0x10);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    EXPECT_TRUE(reader->record.xl_info & kXlrBkpBlockImage);
    EXPECT_EQ(reader->backup_blocks.size(), 1u);
    XLogReaderFree(reader);
}

// Registering a buffer with is_fpw=false does NOT set kXlrBkpBlockImage.
TEST_F(XLogFpwTest, NoFpwFlagWhenBufferNotFpw) {
    std::vector<uint8_t> page(8, 0xAB);
    XLogBeginInsert();
    XLogRegisterBuffer(0, page.data(), page.size(), /*is_fpw=*/false);
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0x10);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    EXPECT_FALSE(reader->record.xl_info & kXlrBkpBlockImage);
    EXPECT_TRUE(reader->backup_blocks.empty());
    XLogReaderFree(reader);
}

// --------------------------------------------------------------------------
// Backup block content round-trip
// --------------------------------------------------------------------------

// Backup block page_data matches what was registered.
TEST_F(XLogFpwTest, BackupBlockContentMatches) {
    std::vector<uint8_t> page = {0xDE, 0xAD, 0xBE, 0xEF,
                                  0xCA, 0xFE, 0xBA, 0xBE};
    XLogBeginInsert();
    XLogRegisterBuffer(0, page.data(), page.size(), /*is_fpw=*/true);
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    ASSERT_EQ(reader->backup_blocks.size(), 1u);
    EXPECT_EQ(reader->backup_blocks[0].block_id, 0);
    EXPECT_EQ(reader->backup_blocks[0].page_data, page);
    XLogReaderFree(reader);
}

// block_id is preserved through the round-trip.
TEST_F(XLogFpwTest, BackupBlockIdPreserved) {
    std::vector<uint8_t> page(4, 0x42);
    XLogBeginInsert();
    XLogRegisterBuffer(7, page.data(), page.size(), /*is_fpw=*/true);
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    ASSERT_EQ(reader->backup_blocks.size(), 1u);
    EXPECT_EQ(reader->backup_blocks[0].block_id, 7);
    XLogReaderFree(reader);
}

// Multiple FPW buffers produce multiple backup blocks in order.
TEST_F(XLogFpwTest, MultipleBackupBlocks) {
    std::vector<uint8_t> page0 = {0x00, 0x01, 0x02};
    std::vector<uint8_t> page1 = {0x10, 0x11, 0x12, 0x13, 0x14};
    std::vector<uint8_t> page2 = {0x20};

    XLogBeginInsert();
    XLogRegisterBuffer(0, page0.data(), page0.size(), /*is_fpw=*/true);
    XLogRegisterBuffer(1, page1.data(), page1.size(), /*is_fpw=*/true);
    XLogRegisterBuffer(2, page2.data(), page2.size(), /*is_fpw=*/true);
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    ASSERT_EQ(reader->backup_blocks.size(), 3u);
    EXPECT_EQ(reader->backup_blocks[0].block_id, 0);
    EXPECT_EQ(reader->backup_blocks[0].page_data, page0);
    EXPECT_EQ(reader->backup_blocks[1].block_id, 1);
    EXPECT_EQ(reader->backup_blocks[1].page_data, page1);
    EXPECT_EQ(reader->backup_blocks[2].block_id, 2);
    EXPECT_EQ(reader->backup_blocks[2].page_data, page2);
    XLogReaderFree(reader);
}

// --------------------------------------------------------------------------
// Main data separation
// --------------------------------------------------------------------------

// Main data is correctly separated from backup blocks.
TEST_F(XLogFpwTest, MainDataSeparatedFromBackupBlocks) {
    std::vector<uint8_t> page(16, 0x77);
    uint32_t main_payload = 0xCAFED00D;

    XLogBeginInsert();
    XLogRegisterBuffer(0, page.data(), page.size(), /*is_fpw=*/true);
    XLogRegisterData(&main_payload, sizeof(main_payload));
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0x20);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    EXPECT_TRUE(reader->record.xl_info & kXlrBkpBlockImage);
    ASSERT_EQ(reader->backup_blocks.size(), 1u);
    EXPECT_EQ(reader->backup_blocks[0].page_data, page);

    // main_data should contain only the 4-byte payload, not the page image.
    EXPECT_EQ(reader->main_data.size(), sizeof(main_payload));
    uint32_t read_back = 0;
    std::memcpy(&read_back, reader->main_data.data(), sizeof(read_back));
    EXPECT_EQ(read_back, main_payload);
    XLogReaderFree(reader);
}

// FPW record with no main data: backup blocks present, main_data empty.
TEST_F(XLogFpwTest, FpwRecordWithNoMainData) {
    std::vector<uint8_t> page(8, 0x99);
    XLogBeginInsert();
    XLogRegisterBuffer(0, page.data(), page.size(), /*is_fpw=*/true);
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    ASSERT_EQ(reader->backup_blocks.size(), 1u);
    EXPECT_TRUE(reader->main_data.empty());
    XLogReaderFree(reader);
}

// Mixed: one FPW buffer + one non-FPW buffer + main data.
TEST_F(XLogFpwTest, MixedFpwAndNonFpwBuffers) {
    std::vector<uint8_t> fpw_page = {0xAA, 0xBB, 0xCC, 0xDD};
    std::vector<uint8_t> non_fpw_page = {0x11, 0x22};
    uint16_t main_val = 0xBEEF;

    XLogBeginInsert();
    XLogRegisterBuffer(0, fpw_page.data(), fpw_page.size(), /*is_fpw=*/true);
    XLogRegisterBuffer(1, non_fpw_page.data(), non_fpw_page.size(),
                       /*is_fpw=*/false);
    XLogRegisterData(&main_val, sizeof(main_val));
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    EXPECT_TRUE(reader->record.xl_info & kXlrBkpBlockImage);
    // Only the FPW buffer becomes a backup block.
    ASSERT_EQ(reader->backup_blocks.size(), 1u);
    EXPECT_EQ(reader->backup_blocks[0].block_id, 0);
    EXPECT_EQ(reader->backup_blocks[0].page_data, fpw_page);
    // Main data is intact.
    EXPECT_EQ(reader->main_data.size(), sizeof(main_val));
    uint16_t read_back = 0;
    std::memcpy(&read_back, reader->main_data.data(), sizeof(read_back));
    EXPECT_EQ(read_back, main_val);
    XLogReaderFree(reader);
}

// --------------------------------------------------------------------------
// Edge cases
// --------------------------------------------------------------------------

// Re-registering the same block_id replaces the previous entry.
TEST_F(XLogFpwTest, ReplaceExistingBlock) {
    std::vector<uint8_t> page_v1(4, 0x01);
    std::vector<uint8_t> page_v2(6, 0x02);

    XLogBeginInsert();
    XLogRegisterBuffer(0, page_v1.data(), page_v1.size(), /*is_fpw=*/true);
    // Same block_id — should replace.
    XLogRegisterBuffer(0, page_v2.data(), page_v2.size(), /*is_fpw=*/true);
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    ASSERT_EQ(reader->backup_blocks.size(), 1u);
    EXPECT_EQ(reader->backup_blocks[0].page_data, page_v2);
    XLogReaderFree(reader);
}

// Registering a buffer with page_len=0 is ignored (no backup block).
TEST_F(XLogFpwTest, EmptyPageDataIgnored) {
    XLogBeginInsert();
    XLogRegisterBuffer(0, nullptr, 0, /*is_fpw=*/true);
    uint32_t payload = 0x12345678;
    XLogRegisterData(&payload, sizeof(payload));
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    EXPECT_FALSE(reader->record.xl_info & kXlrBkpBlockImage);
    EXPECT_TRUE(reader->backup_blocks.empty());
    EXPECT_EQ(reader->main_data.size(), sizeof(payload));
    XLogReaderFree(reader);
}

// --------------------------------------------------------------------------
// Recovery
// --------------------------------------------------------------------------

// Recovery replays an FPW record: redo callback receives correct main_data.
TEST_F(XLogFpwTest, RecoveryReplaysFpwRecordMainData) {
    std::vector<uint8_t> page(32, 0x5A);
    uint32_t main_payload = 0xFEEDFACE;

    XLogBeginInsert();
    XLogRegisterBuffer(0, page.data(), page.size(), /*is_fpw=*/true);
    XLogRegisterData(&main_payload, sizeof(main_payload));
    XLogInsert(kRmgrHeapId, 0);

    // Register a redo callback that captures main_data.
    uint32_t redo_value = 0;
    pgcpp::transaction::RegisterRmgrRedo(
        kRmgrHeapId,
        [&](const XLogRecord&, const uint8_t* data, std::size_t len,
            XLogRecPtr) {
            ASSERT_EQ(len, sizeof(uint32_t));
            std::memcpy(&redo_value, data, sizeof(redo_value));
        });

    RecoveryStats stats = PerformCrashRecovery();
    EXPECT_EQ(stats.records_replayed, 1u);
    EXPECT_EQ(redo_value, main_payload);
}

}  // namespace
