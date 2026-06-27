// smgr_test.cpp — Unit tests for the storage manager (M6 Task 6.1).
//
// Tests SmgrRelation creation, file I/O (read/write/extend), block counting,
// truncation, and the smgr hash table. Uses a temporary directory for file
// storage to avoid polluting the workspace.

#include "mytoydb/storage/smgr.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/storage/bufpage.hpp"

using mytoydb::catalog::Oid;
using mytoydb::memory::AllocSetContext;
using mytoydb::storage::BlockNumber;
using mytoydb::storage::ForkNumber;
using mytoydb::storage::GetStorageBaseDir;
using mytoydb::storage::kBlckSz;
using mytoydb::storage::kInvalidBlockNumber;
using mytoydb::storage::kPageHeaderSize;
using mytoydb::storage::PageInit;
using mytoydb::storage::RelFileNode;
using mytoydb::storage::RelFileNodeBackend;
using mytoydb::storage::SetStorageBaseDir;
using mytoydb::storage::smgrclose;
using mytoydb::storage::smgrcloseall;
using mytoydb::storage::smgrcreate;
using mytoydb::storage::smgrexists;
using mytoydb::storage::smgrextend;
using mytoydb::storage::smgrimmedsync;
using mytoydb::storage::smgrnblocks;
using mytoydb::storage::smgropen;
using mytoydb::storage::smgrread;
using mytoydb::storage::SmgrRelation;
using mytoydb::storage::SmgrRelationData;
using mytoydb::storage::smgrrelease;
using mytoydb::storage::smgrtruncate;
using mytoydb::storage::smgrwrite;

namespace {

class SmgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("smgr_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        // Create a unique temp directory for this test run.
        test_dir_ = "/tmp/mytoydb_smgr_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);

        // Clean up any leftover from previous runs.
        RunShell("rm -rf " + test_dir_);
    }

    void TearDown() override {
        smgrcloseall();
        RunShell("rm -rf " + test_dir_);

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: create a relation file and return an open SmgrRelation.
    SmgrRelation CreateTestRelation(Oid rel_node) {
        RelFileNodeBackend rnode;
        rnode.node.spc_node = 0;
        rnode.node.db_node = 16384;
        rnode.node.rel_node = rel_node;
        rnode.backend = 0;

        SmgrRelation reln = smgropen(rnode);
        smgrcreate(reln, ForkNumber::kMain, false);
        return reln;
    }

    AllocSetContext* context_ = nullptr;
    std::string test_dir_;

private:
    static void RunShell(const std::string& cmd) {
        // Use system() for test setup/teardown only.
        std::system(cmd.c_str());
    }

    static int getpid() { return static_cast<int>(::getpid()); }
};

}  // namespace

// --- smgropen / smgrclose tests ---

TEST_F(SmgrTest, SmgrOpenReturnsSameRelationForSameRnode) {
    RelFileNodeBackend rnode;
    rnode.node.spc_node = 0;
    rnode.node.db_node = 16384;
    rnode.node.rel_node = 100;
    rnode.backend = 0;

    SmgrRelation reln1 = smgropen(rnode);
    SmgrRelation reln2 = smgropen(rnode);

    EXPECT_EQ(reln1, reln2);
}

TEST_F(SmgrTest, SmgrOpenReturnsDifferentRelationForDifferentRnode) {
    RelFileNodeBackend rnode1;
    rnode1.node.rel_node = 100;
    RelFileNodeBackend rnode2;
    rnode2.node.rel_node = 200;

    SmgrRelation reln1 = smgropen(rnode1);
    SmgrRelation reln2 = smgropen(rnode2);

    EXPECT_NE(reln1, reln2);
}

TEST_F(SmgrTest, SmgrCloseRemovesFromHashTable) {
    RelFileNodeBackend rnode;
    rnode.node.spc_node = 0;
    rnode.node.db_node = 16384;
    rnode.node.rel_node = 100;
    rnode.backend = 0;

    SmgrRelation reln = smgropen(rnode);
    smgrclose(reln);

    // Opening again should create a new entry with clean state
    // (no open file descriptors).
    SmgrRelation reln2 = smgropen(rnode);
    // The new relation should have no open segments.
    EXPECT_TRUE(reln2->md_fd[static_cast<int>(ForkNumber::kMain)].empty());
    smgrclose(reln2);
}

// --- smgrcreate / smgrnblocks tests ---

TEST_F(SmgrTest, SmgrCreateCreatesEmptyFile) {
    SmgrRelation reln = CreateTestRelation(100);

    // A newly created file has 0 blocks.
    EXPECT_EQ(smgrnblocks(reln, ForkNumber::kMain), 0);
}

TEST_F(SmgrTest, SmgrCreateErrorsOnExistingFile) {
    SmgrRelation reln = CreateTestRelation(100);

    // Creating again should error (file already exists).
    bool caught = false;
    PG_TRY() {
        smgrcreate(reln, ForkNumber::kMain, false);
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
}

// --- smgrextend / smgrread tests ---

TEST_F(SmgrTest, SmgrExtendAddsBlock) {
    SmgrRelation reln = CreateTestRelation(100);

    // Write a block of data.
    char write_buf[kBlckSz];
    std::memset(write_buf, 0, kBlckSz);
    PageInit(write_buf, kBlckSz, 0);

    smgrextend(reln, ForkNumber::kMain, 0, write_buf, false);

    EXPECT_EQ(smgrnblocks(reln, ForkNumber::kMain), 1);
}

TEST_F(SmgrTest, SmgrReadReturnsWrittenData) {
    SmgrRelation reln = CreateTestRelation(100);

    // Write a block with known content.
    char write_buf[kBlckSz];
    std::memset(write_buf, 0, kBlckSz);
    PageInit(write_buf, kBlckSz, 0);
    // Write some data into the page content area.
    std::memcpy(write_buf + kPageHeaderSize, "test data", 9);

    smgrextend(reln, ForkNumber::kMain, 0, write_buf, false);

    // Read it back.
    char read_buf[kBlckSz];
    smgrread(reln, ForkNumber::kMain, 0, read_buf);

    // Verify the content matches.
    EXPECT_EQ(std::memcmp(write_buf, read_buf, kBlckSz), 0);
}

TEST_F(SmgrTest, SmgrWriteUpdatesExistingBlock) {
    SmgrRelation reln = CreateTestRelation(100);

    // Extend with an initial block.
    char buf[kBlckSz];
    std::memset(buf, 0, kBlckSz);
    PageInit(buf, kBlckSz, 0);
    smgrextend(reln, ForkNumber::kMain, 0, buf, false);

    // Write new content to block 0.
    std::memcpy(buf + kPageHeaderSize, "updated", 7);
    smgrwrite(reln, ForkNumber::kMain, 0, buf, false);

    // Read back and verify.
    char read_buf[kBlckSz];
    smgrread(reln, ForkNumber::kMain, 0, read_buf);
    EXPECT_EQ(std::memcmp(buf, read_buf, kBlckSz), 0);
}

TEST_F(SmgrTest, SmgrExtendMultipleBlocks) {
    SmgrRelation reln = CreateTestRelation(100);

    char buf[kBlckSz];
    for (BlockNumber i = 0; i < 5; ++i) {
        std::memset(buf, static_cast<int>(i), kBlckSz);
        PageInit(buf, kBlckSz, 0);
        smgrextend(reln, ForkNumber::kMain, i, buf, false);
    }

    EXPECT_EQ(smgrnblocks(reln, ForkNumber::kMain), 5);

    // Read each block and verify content.
    for (BlockNumber i = 0; i < 5; ++i) {
        char read_buf[kBlckSz];
        smgrread(reln, ForkNumber::kMain, i, read_buf);
        auto* phdr = reinterpret_cast<mytoydb::storage::PageHeader>(read_buf);
        // The page header should be initialized.
        EXPECT_EQ(phdr->pd_lower, kPageHeaderSize);
    }
}

// --- smgrtruncate tests ---

TEST_F(SmgrTest, SmgrTruncateRemovesBlocks) {
    SmgrRelation reln = CreateTestRelation(100);

    // Create 5 blocks.
    char buf[kBlckSz];
    for (BlockNumber i = 0; i < 5; ++i) {
        std::memset(buf, 0, kBlckSz);
        PageInit(buf, kBlckSz, 0);
        smgrextend(reln, ForkNumber::kMain, i, buf, false);
    }
    EXPECT_EQ(smgrnblocks(reln, ForkNumber::kMain), 5);

    // Truncate to 2 blocks.
    smgrtruncate(reln, ForkNumber::kMain, 2);
    EXPECT_EQ(smgrnblocks(reln, ForkNumber::kMain), 2);
}

TEST_F(SmgrTest, SmgrTruncateToZero) {
    SmgrRelation reln = CreateTestRelation(100);

    char buf[kBlckSz];
    std::memset(buf, 0, kBlckSz);
    PageInit(buf, kBlckSz, 0);
    smgrextend(reln, ForkNumber::kMain, 0, buf, false);
    EXPECT_EQ(smgrnblocks(reln, ForkNumber::kMain), 1);

    smgrtruncate(reln, ForkNumber::kMain, 0);
    EXPECT_EQ(smgrnblocks(reln, ForkNumber::kMain), 0);
}

// --- smgrimmedsync tests ---

TEST_F(SmgrTest, SmgrImmedsyncDoesNotError) {
    SmgrRelation reln = CreateTestRelation(100);

    char buf[kBlckSz];
    std::memset(buf, 0, kBlckSz);
    PageInit(buf, kBlckSz, 0);
    smgrextend(reln, ForkNumber::kMain, 0, buf, false);

    // Just verify it doesn't error.
    smgrimmedsync(reln, ForkNumber::kMain);
    SUCCEED();
}

// --- Persistence across smgrclose/smgropen ---

TEST_F(SmgrTest, DataPersistsAcrossCloseAndOpen) {
    SmgrRelation reln = CreateTestRelation(100);

    char write_buf[kBlckSz];
    std::memset(write_buf, 0, kBlckSz);
    PageInit(write_buf, kBlckSz, 0);
    std::memcpy(write_buf + kPageHeaderSize, "persistent", 10);
    smgrextend(reln, ForkNumber::kMain, 0, write_buf, false);

    // Close and reopen.
    smgrclose(reln);
    RelFileNodeBackend rnode;
    rnode.node.spc_node = 0;
    rnode.node.db_node = 16384;
    rnode.node.rel_node = 100;
    rnode.backend = 0;
    reln = smgropen(rnode);

    // Read the data back.
    char read_buf[kBlckSz];
    smgrread(reln, ForkNumber::kMain, 0, read_buf);
    EXPECT_EQ(std::memcmp(write_buf, read_buf, kBlckSz), 0);
}

// --- M6 P0 extension tests (Task 15.7.4) ---

TEST_F(SmgrTest, smgrexists_ReturnsTrueAfterCreate) {
    SmgrRelation reln = CreateTestRelation(200);

    EXPECT_TRUE(smgrexists(reln, ForkNumber::kMain));
}

TEST_F(SmgrTest, smgrexists_ReturnsFalseBeforeCreate) {
    // Open a relation without creating its file on disk.
    RelFileNodeBackend rnode;
    rnode.node.spc_node = 0;
    rnode.node.db_node = 16384;
    rnode.node.rel_node = 999;
    rnode.backend = 0;
    SmgrRelation reln = smgropen(rnode);

    EXPECT_FALSE(smgrexists(reln, ForkNumber::kMain));

    smgrclose(reln);
}

TEST_F(SmgrTest, smgrrelease_ReleasesFDs) {
    SmgrRelation reln = CreateTestRelation(300);

    // Extend opens the segment FD.
    char buf[kBlckSz];
    std::memset(buf, 0, kBlckSz);
    PageInit(buf, kBlckSz, 0);
    smgrextend(reln, ForkNumber::kMain, 0, buf, false);
    EXPECT_FALSE(reln->md_fd[static_cast<int>(ForkNumber::kMain)].empty());

    // Release closes all FDs but keeps the SmgrRelation entry.
    smgrrelease(reln);

    EXPECT_TRUE(reln->md_fd[static_cast<int>(ForkNumber::kMain)].empty());
    // The file still exists on disk.
    EXPECT_TRUE(smgrexists(reln, ForkNumber::kMain));
    // Reading after release reopens the FD transparently.
    char read_buf[kBlckSz];
    smgrread(reln, ForkNumber::kMain, 0, read_buf);
    EXPECT_EQ(std::memcmp(buf, read_buf, kBlckSz), 0);
}

TEST_F(SmgrTest, mdunlink_RemovesFiles) {
    SmgrRelation reln = CreateTestRelation(400);

    char buf[kBlckSz];
    std::memset(buf, 0, kBlckSz);
    PageInit(buf, kBlckSz, 0);
    smgrextend(reln, ForkNumber::kMain, 0, buf, false);
    EXPECT_TRUE(smgrexists(reln, ForkNumber::kMain));

    // Unlink the main fork via the md-level method.
    reln->mdunlink(ForkNumber::kMain, false);

    EXPECT_FALSE(smgrexists(reln, ForkNumber::kMain));
    // The SmgrRelation entry survives mdunlink (only smgrdounlinkall removes
    // the entry from the hash table).
    EXPECT_EQ(smgrnblocks(reln, ForkNumber::kMain), 0);

    smgrclose(reln);
}
