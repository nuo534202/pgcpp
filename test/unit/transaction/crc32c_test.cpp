// crc32c_test.cpp — Unit tests for CRC32C verification (Step 3).
//
// Tests:
//   - CRC32C known test vectors (empty, "123456789", etc.)
//   - Incremental vs one-shot produce same result
//   - Software and SSE4.2 produce identical results (implicit via runtime
//     selection; we verify correctness against known vectors)
//   - WAL record CRC: XLogInsert computes correct CRC, XLogReader verifies it
//   - Corrupt record: modified payload triggers crc_mismatch
//   - Corrupt header: modified xl_tot_len triggers crc_mismatch

#include "transaction/crc32c.hpp"

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

using pgcpp::transaction::Crc32C;
using pgcpp::transaction::Crc32CCompute;
using pgcpp::transaction::GetCurrentTransactionIdIfAny;
using pgcpp::transaction::GetWalBuffer;
using pgcpp::transaction::GetXLogInsertRecPtr;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::InitializeWal;
using pgcpp::transaction::kBootstrapTransactionId;
using pgcpp::transaction::kInvalidTransactionId;
using pgcpp::transaction::kInvalidXLogRecPtr;
using pgcpp::transaction::kRmgrHeapId;
using pgcpp::transaction::kSizeofXlogRecord;
using pgcpp::transaction::PerformCrashRecovery;
using pgcpp::transaction::RecoveryStats;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::ResetWal;
using pgcpp::transaction::ResetXlogInsertState;
using pgcpp::transaction::SetWalDirectory;
using pgcpp::transaction::ShutdownWal;
using pgcpp::transaction::XLogBeginInsert;
using pgcpp::transaction::XLogFlush;
using pgcpp::transaction::XLogInsert;
using pgcpp::transaction::XLogReaderAlloc;
using pgcpp::transaction::XLogReaderFree;
using pgcpp::transaction::XLogReaderState;
using pgcpp::transaction::XLogReadRecordAt;
using pgcpp::transaction::XLogRecord;
using pgcpp::transaction::XLogRecPtr;
using pgcpp::transaction::XLogRegisterData;
using pgcpp::transaction::XLogResetInsert;
using pgcpp::transaction::XLogWriteRaw;

namespace {

// ===========================================================================
// CRC32C known test vectors
// ===========================================================================

TEST(Crc32CTest, KnownVector_EmptyInput) {
    // CRC32C of empty input is 0x00000000 (after finalize: 0xFFFFFFFF ^ 0xFFFFFFFF).
    EXPECT_EQ(Crc32CCompute("", 0), 0u);
}

TEST(Crc32CTest, KnownVector_123456789) {
    // Standard CRC32C test vector: "123456789" -> 0xE3069283.
    const char* data = "123456789";
    EXPECT_EQ(Crc32CCompute(data, 9), 0xE3069283u);
}

TEST(Crc32CTest, KnownVector_SingleByte) {
    // CRC32C of a single zero byte.
    uint8_t byte = 0;
    EXPECT_EQ(Crc32CCompute(&byte, 1), 0x527D5351u);
}

TEST(Crc32CTest, KnownVector_AllZeros_16Bytes) {
    std::vector<uint8_t> zeros(16, 0);
    // Verified against a Python reference implementation (reflected Castagnoli
    // polynomial 0x82F63B78, init/final 0xFFFFFFFF).
    EXPECT_EQ(Crc32CCompute(zeros.data(), 16), 0x42709AEAu);
}

// ===========================================================================
// Incremental vs one-shot
// ===========================================================================

TEST(Crc32CTest, IncrementalMatchesOneShot) {
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                            0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};

    uint32_t one_shot = Crc32CCompute(data, sizeof(data));

    // Split into 3 chunks of different sizes.
    Crc32C crc;
    crc.Update(data, 3);
    crc.Update(data + 3, 5);
    crc.Update(data + 8, sizeof(data) - 8);
    uint32_t incremental = crc.Finalize();

    EXPECT_EQ(incremental, one_shot);
}

TEST(Crc32CTest, ResetAllowsReuse) {
    Crc32C crc;
    crc.Update("abc", 3);
    crc.Finalize();

    crc.Reset();
    crc.Update("123456789", 9);
    EXPECT_EQ(crc.Finalize(), 0xE3069283u);
}

TEST(Crc32CTest, UpdateWithNullIsNoOp) {
    Crc32C crc;
    crc.Update(nullptr, 10);
    EXPECT_EQ(crc.Finalize(), 0u);  // empty input -> 0
}

// ===========================================================================
// WAL record CRC verification
// ===========================================================================

class WalCrcTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("wal_crc_test");
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

// XLogInsert computes a non-zero CRC.
TEST_F(WalCrcTest, InsertComputesNonZeroCrc) {
    uint32_t payload = 0xDEADBEEF;
    XLogBeginInsert();
    XLogRegisterData(&payload, sizeof(payload));
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    // Read the record back and check xl_crc is non-zero.
    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    EXPECT_NE(reader->record.xl_crc, 0u);
    EXPECT_FALSE(reader->crc_mismatch);
    XLogReaderFree(reader);
}

// XLogReader successfully verifies a correctly-written record.
TEST_F(WalCrcTest, ReaderVerifiesCorrectCrc) {
    uint32_t payload = 0x12345678;
    XLogBeginInsert();
    XLogRegisterData(&payload, sizeof(payload));
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0x10);

    auto* reader = XLogReaderAlloc();
    ASSERT_TRUE(XLogReadRecordAt(reader, lsn));
    EXPECT_FALSE(reader->crc_mismatch);
    EXPECT_EQ(reader->record.xl_info, 0x10);
    EXPECT_EQ(reader->main_data.size(), sizeof(payload));

    uint32_t read_back = 0;
    std::memcpy(&read_back, reader->main_data.data(), sizeof(read_back));
    EXPECT_EQ(read_back, payload);
    XLogReaderFree(reader);
}

// Corrupting a payload byte triggers crc_mismatch.
TEST_F(WalCrcTest, CorruptPayloadTriggersMismatch) {
    uint32_t payload = 0xCAFEBABE;
    XLogBeginInsert();
    XLogRegisterData(&payload, sizeof(payload));
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    // Corrupt the first payload byte (offset = lsn + kSizeofXlogRecord).
    auto& buffer = const_cast<std::vector<uint8_t>&>(GetWalBuffer());
    buffer[lsn + kSizeofXlogRecord] ^= 0xFF;

    auto* reader = XLogReaderAlloc();
    EXPECT_FALSE(XLogReadRecordAt(reader, lsn));
    EXPECT_TRUE(reader->crc_mismatch);
    EXPECT_EQ(reader->bad_lsn, lsn);
    XLogReaderFree(reader);
}

// Corrupting the xl_info header field triggers crc_mismatch.
TEST_F(WalCrcTest, CorruptHeaderTriggersMismatch) {
    uint32_t payload = 0xAABBCCDD;
    XLogBeginInsert();
    XLogRegisterData(&payload, sizeof(payload));
    XLogRecPtr lsn = XLogInsert(kRmgrHeapId, 0);

    // Corrupt xl_info (offset 16 in the header, at lsn + 16).
    auto& buffer = const_cast<std::vector<uint8_t>&>(GetWalBuffer());
    buffer[lsn + 16] ^= 0xFF;

    auto* reader = XLogReaderAlloc();
    EXPECT_FALSE(XLogReadRecordAt(reader, lsn));
    EXPECT_TRUE(reader->crc_mismatch);
    EXPECT_EQ(reader->bad_lsn, lsn);
    XLogReaderFree(reader);
}

// Multiple records in sequence all have valid CRCs.
TEST_F(WalCrcTest, MultipleRecordsAllHaveValidCrc) {
    std::vector<uint32_t> payloads = {0x11111111, 0x22222222, 0x33333333};
    std::vector<XLogRecPtr> lsns;

    for (uint32_t p : payloads) {
        XLogBeginInsert();
        XLogRegisterData(&p, sizeof(p));
        lsns.push_back(XLogInsert(kRmgrHeapId, 0));
    }

    auto* reader = XLogReaderAlloc();
    for (std::size_t i = 0; i < lsns.size(); i++) {
        ASSERT_TRUE(XLogReadRecordAt(reader, lsns[i])) << "record " << i << " failed";
        EXPECT_FALSE(reader->crc_mismatch);
        EXPECT_EQ(reader->main_data.size(), sizeof(uint32_t));

        uint32_t read_back = 0;
        std::memcpy(&read_back, reader->main_data.data(), sizeof(read_back));
        EXPECT_EQ(read_back, payloads[i]);
    }
    XLogReaderFree(reader);
}

// Crash recovery replays records with valid CRCs.
TEST_F(WalCrcTest, CrashRecoveryWithValidCrc) {
    // Write 3 records.
    std::vector<uint32_t> written;
    for (int i = 0; i < 3; i++) {
        uint32_t val = static_cast<uint32_t>(5000 + i);
        XLogBeginInsert();
        XLogRegisterData(&val, sizeof(val));
        XLogInsert(kRmgrHeapId, 0);
        written.push_back(val);
    }

    // Replay and verify.
    std::vector<uint32_t> replayed;
    pgcpp::transaction::RegisterRmgrRedo(
        kRmgrHeapId, [&](const XLogRecord&, const uint8_t* data, std::size_t len, XLogRecPtr) {
            ASSERT_EQ(len, sizeof(uint32_t));
            uint32_t v = 0;
            std::memcpy(&v, data, sizeof(v));
            replayed.push_back(v);
        });
    RecoveryStats stats = PerformCrashRecovery();
    EXPECT_EQ(stats.records_replayed, 3u);
    ASSERT_EQ(replayed.size(), written.size());
    for (std::size_t i = 0; i < written.size(); i++) {
        EXPECT_EQ(replayed[i], written[i]);
    }
}

}  // namespace
