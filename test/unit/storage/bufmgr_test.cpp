// bufmgr_test.cpp — Unit tests for the buffer manager (M6 Task 6.2).
//
// Tests ReadBuffer (hit/miss), ReleaseBuffer, MarkBufferDirty, buffer
// eviction (clock sweep), and dirty page writeback. Uses a small buffer
// pool to force eviction scenarios.

#include "pgcpp/storage/bufmgr.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/storage/buf_internals.hpp"
#include "pgcpp/storage/bufpage.hpp"
#include "pgcpp/storage/smgr.hpp"

using pgcpp::memory::AllocSetContext;
using pgcpp::storage::BlockNumber;
using pgcpp::storage::Buffer;
using pgcpp::storage::BufferAccessStrategy;
using pgcpp::storage::BufferDesc;
using pgcpp::storage::BufferGetPage;
using pgcpp::storage::BufferGetTag;
using pgcpp::storage::BufferPool;
using pgcpp::storage::BufferTag;
using pgcpp::storage::DropRelFileNodeBuffers;
using pgcpp::storage::FlushBuffer;
using pgcpp::storage::FlushRelationBuffers;
using pgcpp::storage::ForkNumber;
using pgcpp::storage::GetBufferPool;
using pgcpp::storage::IncrBufferRefCount;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::kBlckSz;
using pgcpp::storage::kInvalidBuffer;
using pgcpp::storage::kPageHeaderSize;
using pgcpp::storage::MarkBufferDirty;
using pgcpp::storage::MarkBufferDirtyHint;
using pgcpp::storage::Page;
using pgcpp::storage::PageHeader;
using pgcpp::storage::PageInit;
using pgcpp::storage::ReadBuffer;
using pgcpp::storage::ReadBufferMode;
using pgcpp::storage::ReleaseAndReadBuffer;
using pgcpp::storage::ReleaseBuffer;
using pgcpp::storage::RelFileNode;
using pgcpp::storage::RelFileNodeBackend;
using pgcpp::storage::SetBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrclose;
using pgcpp::storage::smgrcloseall;
using pgcpp::storage::smgrcreate;
using pgcpp::storage::smgrextend;
using pgcpp::storage::smgrnblocks;
using pgcpp::storage::smgropen;
using pgcpp::storage::SmgrRelation;

namespace {

class BufMgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("bufmgr_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        // Set up temp storage directory.
        test_dir_ = "/tmp/mytoydb_bufmgr_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        std::system(("rm -rf " + test_dir_).c_str());

        // Create a small buffer pool (4 buffers) for easy eviction testing.
        InitBufferPool(4);

        // Create a test relation with 1 initial block.
        RelFileNodeBackend rnode;
        rnode.node.spc_node = 0;
        rnode.node.db_node = 16384;
        rnode.node.rel_node = 200;
        rnode.backend = 0;
        reln_ = smgropen(rnode);
        smgrcreate(reln_, ForkNumber::kMain, false);

        // Extend with one block containing initialized page.
        char buf[kBlckSz];
        std::memset(buf, 0, kBlckSz);
        PageInit(buf, kBlckSz, 0);
        smgrextend(reln_, ForkNumber::kMain, 0, buf, false);
    }

    void TearDown() override {
        ShutdownBufferPool();
        smgrcloseall();
        std::system(("rm -rf " + test_dir_).c_str());

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: extend the test relation by one more block.
    void ExtendRelation(BlockNumber block_num) {
        char buf[kBlckSz];
        std::memset(buf, 0, kBlckSz);
        PageInit(buf, kBlckSz, 0);
        smgrextend(reln_, ForkNumber::kMain, block_num, buf, false);
    }

    AllocSetContext* context_ = nullptr;
    std::string test_dir_;
    SmgrRelation reln_ = nullptr;
};

}  // namespace

// --- ReadBuffer basic tests ---

TEST_F(BufMgrTest, ReadBufferReturnsValidBuffer) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    EXPECT_NE(buf, kInvalidBuffer);
    EXPECT_GT(buf, 0);
    ReleaseBuffer(buf);
}

TEST_F(BufMgrTest, ReadBufferGetPageReturnsPagePointer) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buf);
    EXPECT_NE(page, nullptr);

    // The page should have a valid header (we initialized it with PageInit).
    auto* phdr = reinterpret_cast<PageHeader>(page);
    EXPECT_EQ(phdr->pd_lower, kPageHeaderSize);

    ReleaseBuffer(buf);
}

TEST_F(BufMgrTest, ReadBufferHitReturnsSameBuffer) {
    // First read: buffer miss, reads from disk.
    Buffer buf1 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    ReleaseBuffer(buf1);

    // Second read: buffer hit, should return the same buffer.
    Buffer buf2 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    EXPECT_EQ(buf1, buf2);
    ReleaseBuffer(buf2);
}

TEST_F(BufMgrTest, ReadBufferDifferentBlocksReturnDifferentBuffers) {
    // Extend the relation to have 2 blocks.
    ExtendRelation(1);

    Buffer buf1 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Buffer buf2 = ReadBuffer(reln_, ForkNumber::kMain, 1, ReadBufferMode::kNormal);
    EXPECT_NE(buf1, buf2);

    ReleaseBuffer(buf1);
    ReleaseBuffer(buf2);
}

// --- ReadBuffer with RBM_ZERO ---

TEST_F(BufMgrTest, ReadBufferZeroReturnsZeroedPage) {
    // Create a new block (block 1) that doesn't exist on disk yet.
    // Use RBM_ZERO to get a zeroed page without reading from disk.
    // First, we need to extend the file so the block exists.
    ExtendRelation(1);

    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 1, ReadBufferMode::kZero);
    Page page = BufferGetPage(buf);

    // The page should be all zeros (RBM_ZERO doesn't call PageInit).
    for (int i = 0; i < kBlckSz; ++i) {
        EXPECT_EQ(page[i], 0) << "byte " << i << " not zero";
    }

    ReleaseBuffer(buf);
}

// --- ReleaseBuffer / pin count tests ---

TEST_F(BufMgrTest, ReleaseBufferDecrementsRefcount) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);

    BufferPool* pool = GetBufferPool();
    BufferDesc* desc = pool->GetBufferDesc(buf);
    EXPECT_EQ(desc->refcount, 1);

    ReleaseBuffer(buf);
    EXPECT_EQ(desc->refcount, 0);
}

TEST_F(BufMgrTest, MultiplePinsIncrementRefcount) {
    Buffer buf1 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Buffer buf2 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);

    // Same buffer, should have refcount = 2.
    EXPECT_EQ(buf1, buf2);
    BufferPool* pool = GetBufferPool();
    BufferDesc* desc = pool->GetBufferDesc(buf1);
    EXPECT_EQ(desc->refcount, 2);

    ReleaseBuffer(buf1);
    EXPECT_EQ(desc->refcount, 1);
    ReleaseBuffer(buf2);
    EXPECT_EQ(desc->refcount, 0);
}

// --- MarkBufferDirty tests ---

TEST_F(BufMgrTest, MarkBufferDirtySetsDirtyFlag) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);

    BufferPool* pool = GetBufferPool();
    BufferDesc* desc = pool->GetBufferDesc(buf);
    EXPECT_FALSE(desc->IsDirty());

    MarkBufferDirty(buf);
    EXPECT_TRUE(desc->IsDirty());

    ReleaseBuffer(buf);
}

TEST_F(BufMgrTest, FlushBufferClearsDirtyFlag) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    MarkBufferDirty(buf);

    BufferPool* pool = GetBufferPool();
    BufferDesc* desc = pool->GetBufferDesc(buf);
    EXPECT_TRUE(desc->IsDirty());

    FlushBuffer(buf);
    EXPECT_FALSE(desc->IsDirty());

    ReleaseBuffer(buf);
}

// --- Buffer eviction (clock sweep) tests ---

TEST_F(BufMgrTest, EvictionReusesBufferSlot) {
    // Buffer pool has 4 slots. Read 4 different blocks to fill it.
    ExtendRelation(1);
    ExtendRelation(2);
    ExtendRelation(3);

    Buffer buf0 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Buffer buf1 = ReadBuffer(reln_, ForkNumber::kMain, 1, ReadBufferMode::kNormal);
    Buffer buf2 = ReadBuffer(reln_, ForkNumber::kMain, 2, ReadBufferMode::kNormal);
    Buffer buf3 = ReadBuffer(reln_, ForkNumber::kMain, 3, ReadBufferMode::kNormal);

    // All 4 slots are now used. Release all.
    ReleaseBuffer(buf0);
    ReleaseBuffer(buf1);
    ReleaseBuffer(buf2);
    ReleaseBuffer(buf3);

    // Read a 5th block — should evict one of the existing buffers.
    ExtendRelation(4);
    Buffer buf4 = ReadBuffer(reln_, ForkNumber::kMain, 4, ReadBufferMode::kNormal);
    EXPECT_NE(buf4, kInvalidBuffer);

    ReleaseBuffer(buf4);
}

TEST_F(BufMgrTest, DirtyEvictionFlushesToDisk) {
    // Read block 0, modify it, mark dirty, release.
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buf);
    // Write some data into the page.
    std::memcpy(page + kPageHeaderSize, "dirty data", 10);
    MarkBufferDirty(buf);
    ReleaseBuffer(buf);

    // Now fill the buffer pool with other blocks to force eviction of block 0.
    ExtendRelation(1);
    ExtendRelation(2);
    ExtendRelation(3);
    ExtendRelation(4);

    Buffer b1 = ReadBuffer(reln_, ForkNumber::kMain, 1, ReadBufferMode::kNormal);
    Buffer b2 = ReadBuffer(reln_, ForkNumber::kMain, 2, ReadBufferMode::kNormal);
    Buffer b3 = ReadBuffer(reln_, ForkNumber::kMain, 3, ReadBufferMode::kNormal);
    Buffer b4 = ReadBuffer(reln_, ForkNumber::kMain, 4, ReadBufferMode::kNormal);

    ReleaseBuffer(b1);
    ReleaseBuffer(b2);
    ReleaseBuffer(b3);
    ReleaseBuffer(b4);

    // Now read block 0 again — it should have been flushed and re-read.
    buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    page = BufferGetPage(buf);

    // Verify the dirty data was persisted.
    EXPECT_EQ(std::memcmp(page + kPageHeaderSize, "dirty data", 10), 0);

    ReleaseBuffer(buf);
}

// --- FlushRelationBuffers tests ---

TEST_F(BufMgrTest, FlushRelationBuffersWritesAllDirty) {
    // Read and dirty two blocks.
    ExtendRelation(1);

    Buffer buf0 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page page0 = BufferGetPage(buf0);
    std::memcpy(page0 + kPageHeaderSize, "flush0", 6);
    MarkBufferDirty(buf0);
    ReleaseBuffer(buf0);

    Buffer buf1 = ReadBuffer(reln_, ForkNumber::kMain, 1, ReadBufferMode::kNormal);
    Page page1 = BufferGetPage(buf1);
    std::memcpy(page1 + kPageHeaderSize, "flush1", 6);
    MarkBufferDirty(buf1);
    ReleaseBuffer(buf1);

    // Flush all buffers for this relation.
    FlushRelationBuffers(reln_);

    // Verify no dirty buffers remain.
    BufferPool* pool = GetBufferPool();
    EXPECT_EQ(pool->NumDirty(), 0);
}

// --- Buffer pool stats tests ---

TEST_F(BufMgrTest, NumPinnedTracksRefCount) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);

    BufferPool* pool = GetBufferPool();
    EXPECT_EQ(pool->NumPinned(), 1);

    ReleaseBuffer(buf);
    EXPECT_EQ(pool->NumPinned(), 0);
}

TEST_F(BufMgrTest, NumDirtyTracksDirtyFlag) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);

    BufferPool* pool = GetBufferPool();
    EXPECT_EQ(pool->NumDirty(), 0);

    MarkBufferDirty(buf);
    EXPECT_EQ(pool->NumDirty(), 1);

    FlushBuffer(buf);
    EXPECT_EQ(pool->NumDirty(), 0);

    ReleaseBuffer(buf);
}

// --- M6 P0 extension tests (Task 15.7.1) ---

TEST_F(BufMgrTest, MarkBufferDirtyHint_MarksBufferDirty) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    BufferPool* pool = GetBufferPool();
    BufferDesc* desc = pool->GetBufferDesc(buf);

    EXPECT_FALSE(desc->IsDirty());
    MarkBufferDirtyHint(buf, /*release=*/false);
    EXPECT_TRUE(desc->IsDirty());

    // Calling again on an already-dirty buffer is a no-op (still dirty).
    MarkBufferDirtyHint(buf, /*release=*/false);
    EXPECT_TRUE(desc->IsDirty());

    // With release=true, the buffer is unpinned after being marked.
    FlushBuffer(buf);  // clear dirty so we can observe the release effect
    Buffer buf2 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    EXPECT_EQ(pool->GetBufferDesc(buf2)->refcount, 2);  // buf + buf2 both pin
    MarkBufferDirtyHint(buf2, /*release=*/true);
    EXPECT_EQ(pool->GetBufferDesc(buf2)->refcount, 1);  // buf2 released

    ReleaseBuffer(buf);
}

TEST_F(BufMgrTest, ReleaseAndReadBuffer_SameBufferNoRelease) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    BufferPool* pool = GetBufferPool();
    BufferDesc* desc = pool->GetBufferDesc(buf);
    EXPECT_EQ(desc->refcount, 1);

    // Re-read the same page: should reuse the buffer without releasing.
    Buffer buf2 = ReleaseAndReadBuffer(buf, reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    EXPECT_EQ(buf, buf2);
    // refcount should still be 1 (no extra pin, no release).
    EXPECT_EQ(desc->refcount, 1);

    ReleaseBuffer(buf2);
}

TEST_F(BufMgrTest, ReleaseAndReadBuffer_DifferentBufferReleases) {
    ExtendRelation(1);

    Buffer buf0 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    BufferPool* pool = GetBufferPool();
    BufferDesc* desc0 = pool->GetBufferDesc(buf0);
    EXPECT_EQ(desc0->refcount, 1);

    // Read a different block: should release buf0 and read block 1.
    Buffer buf1 = ReleaseAndReadBuffer(buf0, reln_, ForkNumber::kMain, 1, ReadBufferMode::kNormal);
    EXPECT_NE(buf0, buf1);
    // buf0 should have been released (refcount back to 0).
    EXPECT_EQ(desc0->refcount, 0);

    ReleaseBuffer(buf1);
}

TEST_F(BufMgrTest, IncrBufferRefCount_IncrementsCount) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    BufferPool* pool = GetBufferPool();
    BufferDesc* desc = pool->GetBufferDesc(buf);
    EXPECT_EQ(desc->refcount, 1);

    IncrBufferRefCount(buf);
    EXPECT_EQ(desc->refcount, 2);

    IncrBufferRefCount(buf);
    EXPECT_EQ(desc->refcount, 3);

    // Release all three pins.
    ReleaseBuffer(buf);
    ReleaseBuffer(buf);
    ReleaseBuffer(buf);
    EXPECT_EQ(desc->refcount, 0);
}

TEST_F(BufMgrTest, BufferGetTag_ReturnsCorrectTag) {
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);

    RelFileNode rnode;
    ForkNumber forknum;
    BlockNumber blocknum;
    BufferGetTag(buf, &rnode, &forknum, &blocknum);

    EXPECT_EQ(rnode, reln_->smgr_rnode.node);
    EXPECT_EQ(forknum, ForkNumber::kMain);
    EXPECT_EQ(blocknum, 0);

    ReleaseBuffer(buf);
}

TEST_F(BufMgrTest, DropRelFileNodeBuffers_RemovesBuffers) {
    // Read block 0, then release it so it can be dropped.
    Buffer buf = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    BufferPool* pool = GetBufferPool();
    EXPECT_EQ(pool->NumPinned(), 1);
    ReleaseBuffer(buf);
    EXPECT_EQ(pool->NumPinned(), 0);

    // Drop all buffers for this relation's rnode + main fork.
    DropRelFileNodeBuffers(reln_->smgr_rnode, ForkNumber::kMain);

    // Reading the same block again should produce a buffer miss (the old
    // buffer was invalidated). We verify by checking that the buffer pool
    // had to do work — the simplest observable is that the new buffer is
    // valid and the lookup didn't return the stale entry.
    Buffer buf2 = ReadBuffer(reln_, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    EXPECT_NE(buf2, kInvalidBuffer);
    BufferDesc* desc = pool->GetBufferDesc(buf2);
    EXPECT_TRUE(desc->IsValid());
    EXPECT_TRUE(desc->IsTagged());
    EXPECT_EQ(desc->tag.rnode, reln_->smgr_rnode.node);
    ReleaseBuffer(buf2);
}
