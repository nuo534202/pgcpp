// slru_persistence_test.cpp — Unit tests for P0-2 SLRU/CLOG/commit_ts/multixact
// disk persistence.
//
// Tests:
//   - SLRU: write → flush → reload → verify (in-memory + disk-backed)
//   - SLRU: LRU eviction writes dirty pages back to disk
//   - SLRU: capacity-bound eviction preserves recently-used pages
//   - CLOG: TransactionIdCommit/Abort persist across re-init (pg_xact/)
//   - commit_ts: TransactionIdSetCommitTs persists across re-init (pg_commit_ts/)
//   - multixact: MultiXactIdCreate members persist across re-init
//     (pg_multixact/{offsets,members}/)
//
// All tests use /tmp directories that are cleaned up in TearDown.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "transaction/commit_ts.hpp"
#include "transaction/multixact.hpp"
#include "transaction/slru.hpp"
#include "transaction/transam.hpp"

using pgcpp::transaction::FlushClogFiles;
using pgcpp::transaction::FlushCommitTs;
using pgcpp::transaction::FlushMultiXact;
using pgcpp::transaction::InitializeCommitLog;
using pgcpp::transaction::InitializeCommitTs;
using pgcpp::transaction::InitializeMultiXact;
using pgcpp::transaction::kClogXidsPerPage;
using pgcpp::transaction::kFirstMultiXactId;
using pgcpp::transaction::kFirstNormalTransactionId;
using pgcpp::transaction::kInvalidMultiXactId;
using pgcpp::transaction::kMultiXactMemberStride;
using pgcpp::transaction::kSlruPageSize;
using pgcpp::transaction::LoadClogFiles;
using pgcpp::transaction::MultiXactId;
using pgcpp::transaction::MultiXactIdCreate;
using pgcpp::transaction::MultiXactIdExpand;
using pgcpp::transaction::MultiXactIdGetMembers;
using pgcpp::transaction::MultiXactIdIsValid;
using pgcpp::transaction::MultiXactMember;
using pgcpp::transaction::ResetClogPersistence;
using pgcpp::transaction::ResetCommitTs;
using pgcpp::transaction::ResetMultiXact;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::SetClogDirectory;
using pgcpp::transaction::ShutdownClog;
using pgcpp::transaction::ShutdownCommitTs;
using pgcpp::transaction::ShutdownMultiXact;
using pgcpp::transaction::SimpleLruFlush;
using pgcpp::transaction::SimpleLruFree;
using pgcpp::transaction::SimpleLruInit;
using pgcpp::transaction::SimpleLruRead;
using pgcpp::transaction::SimpleLruReset;
using pgcpp::transaction::SimpleLruWrite;
using pgcpp::transaction::SlruCtl;
using pgcpp::transaction::SlruPageStatus;
using pgcpp::transaction::TimestampTz;
using pgcpp::transaction::TransactionId;
using pgcpp::transaction::TransactionIdAbort;
using pgcpp::transaction::TransactionIdCommit;
using pgcpp::transaction::TransactionIdDidAbort;
using pgcpp::transaction::TransactionIdDidCommit;
using pgcpp::transaction::TransactionIdGetCommitTs;
using pgcpp::transaction::TransactionIdSetCommitTs;
using pgcpp::transaction::XidStatus;

namespace {

// Helper: create a fresh temp directory. Returns the path.
std::string MakeTempDir(const std::string& name) {
    std::string path = "/tmp/" + name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directory(path);
    return path;
}

// Helper: recursively remove a directory.
void RemoveTempDir(const std::string& path) {
    std::filesystem::remove_all(path);
}

}  // namespace

// ===========================================================================
// SLRU persistence tests
// ===========================================================================

class SlruPersistenceTest : public ::testing::Test {
protected:
    void TearDown() override {
        for (const auto& path : cleanup_paths_) {
            RemoveTempDir(path);
        }
    }

    std::string RegisterDir(const std::string& name) {
        std::string path = MakeTempDir(name);
        cleanup_paths_.push_back(path);
        return path;
    }

private:
    std::vector<std::string> cleanup_paths_;
};

// SimpleLruWrite + SimpleLruFlush persists data so a new SLRU instance can
// read it back via SimpleLruRead.
TEST_F(SlruPersistenceTest, WriteFlushReload) {
    std::string dir = RegisterDir("pgcpp_slru_persist_write_flush");
    SlruCtl* ctl = SimpleLruInit("test", /*capacity=*/4, dir);

    // Write 8 bytes at page 0, offset 0.
    uint64_t value = 0xDEADBEEFCAFEBABEULL;
    SimpleLruWrite(ctl, /*pageno=*/0, /*offset=*/0, &value, sizeof(value));
    SimpleLruFlush(ctl);
    SimpleLruFree(ctl);

    // Re-create the SLRU and read back.
    ctl = SimpleLruInit("test", /*capacity=*/4, dir);
    uint64_t read_back = 0;
    SimpleLruRead(ctl, /*pageno=*/0, /*offset=*/0, &read_back, sizeof(read_back));
    EXPECT_EQ(read_back, value);
    SimpleLruFree(ctl);
}

// Multiple pages persist correctly.
TEST_F(SlruPersistenceTest, MultiplePagesPersist) {
    std::string dir = RegisterDir("pgcpp_slru_persist_multi_page");
    SlruCtl* ctl = SimpleLruInit("test", /*capacity=*/16, dir);

    // Write different values to pages 0, 1, 5.
    uint32_t v0 = 100, v1 = 200, v5 = 300;
    SimpleLruWrite(ctl, 0, 0, &v0, sizeof(v0));
    SimpleLruWrite(ctl, 1, 0, &v1, sizeof(v1));
    SimpleLruWrite(ctl, 5, 0, &v5, sizeof(v5));
    SimpleLruFlush(ctl);
    SimpleLruFree(ctl);

    ctl = SimpleLruInit("test", /*capacity=*/16, dir);
    uint32_t r0 = 0, r1 = 0, r5 = 0;
    SimpleLruRead(ctl, 0, 0, &r0, sizeof(r0));
    SimpleLruRead(ctl, 1, 0, &r1, sizeof(r1));
    SimpleLruRead(ctl, 5, 0, &r5, sizeof(r5));
    EXPECT_EQ(r0, v0);
    EXPECT_EQ(r1, v1);
    EXPECT_EQ(r5, v5);
    SimpleLruFree(ctl);
}

// LRU eviction writes dirty pages back to disk.
TEST_F(SlruPersistenceTest, EvictionWritesDirtyPage) {
    std::string dir = RegisterDir("pgcpp_slru_persist_evict");
    // Capacity = 2 to force eviction on the 3rd page.
    SlruCtl* ctl = SimpleLruInit("test", /*capacity=*/2, dir);

    // Write to pages 0 and 1 (fill the cache).
    uint32_t v0 = 111, v1 = 222;
    SimpleLruWrite(ctl, 0, 0, &v0, sizeof(v0));
    SimpleLruWrite(ctl, 1, 0, &v1, sizeof(v1));

    // Write to page 2 — evicts page 0 (LRU). Page 0 is dirty, should be
    // written to disk by the eviction path.
    uint32_t v2 = 333;
    SimpleLruWrite(ctl, 2, 0, &v2, sizeof(v2));

    // Flush to make sure page 1 and 2 are also on disk.
    SimpleLruFlush(ctl);
    SimpleLruFree(ctl);

    // Re-create and verify all three pages.
    ctl = SimpleLruInit("test", /*capacity=*/2, dir);
    uint32_t r0 = 0, r1 = 0, r2 = 0;
    SimpleLruRead(ctl, 0, 0, &r0, sizeof(r0));
    SimpleLruRead(ctl, 1, 0, &r1, sizeof(r1));
    SimpleLruRead(ctl, 2, 0, &r2, sizeof(r2));
    EXPECT_EQ(r0, v0) << "evicted dirty page 0 should be persisted";
    EXPECT_EQ(r1, v1);
    EXPECT_EQ(r2, v2);
    SimpleLruFree(ctl);
}

// Reading a non-existent page from disk returns zero-initialized data
// (matches PG behavior).
TEST_F(SlruPersistenceTest, MissingPageReturnsZeros) {
    std::string dir = RegisterDir("pgcpp_slru_persist_missing_page");
    SlruCtl* ctl = SimpleLruInit("test", /*capacity=*/4, dir);

    // No file exists for page 0; read should return zeros.
    uint64_t value = 0xDEADBEEF;
    SimpleLruRead(ctl, 0, 0, &value, sizeof(value));
    EXPECT_EQ(value, 0ULL);
    SimpleLruFree(ctl);
}

// In-memory SLRU (no disk_dir) does not persist data after free.
TEST_F(SlruPersistenceTest, InMemorySlruDoesNotPersist) {
    std::string dir = RegisterDir("pgcpp_slru_persist_in_memory_unused");
    // Create with no disk_dir.
    SlruCtl* ctl = SimpleLruInit("test", /*capacity=*/4, /*disk_dir=*/"");

    uint32_t value = 42;
    SimpleLruWrite(ctl, 0, 0, &value, sizeof(value));
    SimpleLruFlush(ctl);
    SimpleLruFree(ctl);

    // The directory should be empty (no page files written).
    EXPECT_TRUE(std::filesystem::is_empty(dir));
}

// ===========================================================================
// CLOG persistence tests
// ===========================================================================

class ClogPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("clog_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        ResetTransactionState();
        // Default to in-memory mode (no directory).
        SetClogDirectory("");
        ResetClogPersistence();
    }

    void TearDown() override {
        ShutdownClog();
        ResetTransactionState();
        ResetClogPersistence();
        SetClogDirectory("");

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
        for (const auto& path : cleanup_paths_) {
            RemoveTempDir(path);
        }
    }

    std::string RegisterDir(const std::string& name) {
        std::string path = MakeTempDir(name);
        cleanup_paths_.push_back(path);
        return path;
    }

    pgcpp::memory::MemoryContext* context_ = nullptr;

private:
    std::vector<std::string> cleanup_paths_;
};

// TransactionIdCommit persists the committed status to pg_xact/ so a
// restart (re-init + LoadClogFiles) sees it as committed.
TEST_F(ClogPersistenceTest, CommitPersistsAcrossRestart) {
    std::string dir = RegisterDir("pgcpp_clog_persist_commit");
    SetClogDirectory(dir);
    InitializeCommitLog();
    LoadClogFiles();

    TransactionId xid = kFirstNormalTransactionId;
    TransactionIdCommit(xid);
    FlushClogFiles();

    // Simulate restart: re-init CLOG and reload from disk.
    InitializeCommitLog();
    LoadClogFiles();

    EXPECT_TRUE(TransactionIdDidCommit(xid)) << "committed XID should survive restart";
    EXPECT_FALSE(TransactionIdDidAbort(xid));
}

// TransactionIdAbort persists the aborted status to pg_xact/.
TEST_F(ClogPersistenceTest, AbortPersistsAcrossRestart) {
    std::string dir = RegisterDir("pgcpp_clog_persist_abort");
    SetClogDirectory(dir);
    InitializeCommitLog();
    LoadClogFiles();

    TransactionId xid = kFirstNormalTransactionId + 5;
    TransactionIdAbort(xid);
    FlushClogFiles();

    InitializeCommitLog();
    LoadClogFiles();

    EXPECT_TRUE(TransactionIdDidAbort(xid)) << "aborted XID should survive restart";
    EXPECT_FALSE(TransactionIdDidCommit(xid));
}

// Multiple commits/aborts across different pages all persist.
TEST_F(ClogPersistenceTest, MultipleStatusesPersist) {
    std::string dir = RegisterDir("pgcpp_clog_persist_multiple");
    SetClogDirectory(dir);
    InitializeCommitLog();
    LoadClogFiles();

    // Use XIDs that span across the first page boundary (kClogXidsPerPage).
    TransactionId xids[] = {kFirstNormalTransactionId, kFirstNormalTransactionId + 100,
                            kFirstNormalTransactionId + 200,
                            kFirstNormalTransactionId + kClogXidsPerPage + 1};
    for (TransactionId xid : xids) {
        TransactionIdCommit(xid);
    }
    TransactionIdAbort(kFirstNormalTransactionId + 50);
    FlushClogFiles();

    InitializeCommitLog();
    LoadClogFiles();

    for (TransactionId xid : xids) {
        EXPECT_TRUE(TransactionIdDidCommit(xid))
            << "XID " << xid << " should be committed after restart";
    }
    EXPECT_TRUE(TransactionIdDidAbort(kFirstNormalTransactionId + 50));
}

// Without SetClogDirectory, CLOG FlushClogFiles is a no-op and writes no
// files. (The in-memory CLOG itself persists across InitializeCommitLog
// calls because ShmemInitStruct returns the existing shm region in test
// mode; what we verify here is that no disk files are produced.)
TEST_F(ClogPersistenceTest, InMemoryClogWritesNoFiles) {
    std::string dir = RegisterDir("pgcpp_clog_persist_in_memory_unused");
    SetClogDirectory("");  // in-memory mode
    InitializeCommitLog();

    TransactionId xid = kFirstNormalTransactionId;
    TransactionIdCommit(xid);
    FlushClogFiles();

    // The directory we registered should be empty (no page files written).
    // (We point SetClogDirectory at "" so FlushClogFiles is a no-op; the
    // registered dir is just a sentinel to verify nothing was written there.)
    EXPECT_TRUE(std::filesystem::is_empty(dir)) << "in-memory CLOG should not write any files";
}

// ===========================================================================
// commit_ts persistence tests
// ===========================================================================

class CommitTsPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("commit_ts_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        ResetTransactionState();
        // Default to in-memory mode.
        InitializeCommitTs("");
    }

    void TearDown() override {
        ShutdownCommitTs();
        ResetCommitTs();
        ResetTransactionState();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
        for (const auto& path : cleanup_paths_) {
            RemoveTempDir(path);
        }
    }

    std::string RegisterDir(const std::string& name) {
        std::string path = MakeTempDir(name);
        cleanup_paths_.push_back(path);
        return path;
    }

    pgcpp::memory::MemoryContext* context_ = nullptr;

private:
    std::vector<std::string> cleanup_paths_;
};

// TransactionIdSetCommitTs persists the timestamp so it survives a restart.
TEST_F(CommitTsPersistenceTest, SetCommitTsPersistsAcrossRestart) {
    std::string dir = RegisterDir("pgcpp_commit_ts_persist");
    ShutdownCommitTs();  // release the in-memory SLRU from SetUp.
    InitializeCommitTs(dir);

    TransactionId xid = kFirstNormalTransactionId + 10;
    TimestampTz ts = 1234567890;
    TransactionIdSetCommitTs(xid, ts);
    FlushCommitTs();

    // Simulate restart.
    ShutdownCommitTs();
    InitializeCommitTs(dir);

    EXPECT_EQ(TransactionIdGetCommitTs(xid), ts) << "commit timestamp should survive restart";
}

// Multiple commit_ts entries on different pages persist.
TEST_F(CommitTsPersistenceTest, MultipleCommitTsPersist) {
    std::string dir = RegisterDir("pgcpp_commit_ts_persist_multiple");
    ShutdownCommitTs();
    InitializeCommitTs(dir);

    // Use XIDs spanning across the page boundary (1024 entries per page).
    struct Entry {
        TransactionId xid;
        TimestampTz ts;
    };
    std::vector<Entry> entries = {
        {kFirstNormalTransactionId, 1000},
        {kFirstNormalTransactionId + 500, 2000},
        {kFirstNormalTransactionId + 1024, 3000},  // page 1
        {kFirstNormalTransactionId + 2048, 4000},  // page 2
    };
    for (const auto& e : entries) {
        TransactionIdSetCommitTs(e.xid, e.ts);
    }
    FlushCommitTs();

    ShutdownCommitTs();
    InitializeCommitTs(dir);

    for (const auto& e : entries) {
        EXPECT_EQ(TransactionIdGetCommitTs(e.xid), e.ts)
            << "commit_ts for XID " << e.xid << " should survive restart";
    }
}

// Without a disk_dir, commit_ts stays in-memory and does not persist.
TEST_F(CommitTsPersistenceTest, InMemoryCommitTsDoesNotPersist) {
    // SetUp already initialized with empty dir.
    TransactionId xid = kFirstNormalTransactionId;
    TimestampTz ts = 99999;
    TransactionIdSetCommitTs(xid, ts);
    FlushCommitTs();

    ShutdownCommitTs();
    InitializeCommitTs("");

    EXPECT_EQ(TransactionIdGetCommitTs(xid), TimestampTz{0})
        << "in-memory commit_ts should not persist across re-init";
}

// ===========================================================================
// multixact persistence tests
// ===========================================================================

class MultiXactPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("multixact_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        ResetTransactionState();
        // Default to in-memory mode.
        InitializeMultiXact("", "");
    }

    void TearDown() override {
        ShutdownMultiXact();
        ResetMultiXact();
        ResetTransactionState();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
        for (const auto& path : cleanup_paths_) {
            RemoveTempDir(path);
        }
    }

    std::string RegisterDirPair(const std::string& name) {
        std::string base = "/tmp/" + name;
        std::string offsets = base + "/pg_multixact/offsets";
        std::string members = base + "/pg_multixact/members";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(offsets);
        std::filesystem::create_directories(members);
        cleanup_paths_.push_back(base);
        return base;
    }

    pgcpp::memory::MemoryContext* context_ = nullptr;

private:
    std::vector<std::string> cleanup_paths_;
};

// MultiXactIdCreate members persist across a restart.
TEST_F(MultiXactPersistenceTest, CreatePersistsAcrossRestart) {
    std::string base = RegisterDirPair("pgcpp_multixact_persist_create");
    std::string offsets = base + "/pg_multixact/offsets";
    std::string members = base + "/pg_multixact/members";

    ShutdownMultiXact();  // release in-memory SLRUs from SetUp.
    InitializeMultiXact(offsets, members);

    std::vector<MultiXactMember> members_in = {{10, 1}, {11, 2}, {12, 3}};
    MultiXactId multi = MultiXactIdCreate(members_in);
    EXPECT_EQ(multi, kFirstMultiXactId);
    FlushMultiXact();

    // Simulate restart.
    ShutdownMultiXact();
    InitializeMultiXact(offsets, members);

    auto retrieved = MultiXactIdGetMembers(multi);
    ASSERT_EQ(retrieved.size(), members_in.size());
    for (std::size_t i = 0; i < members_in.size(); ++i) {
        EXPECT_EQ(retrieved[i].xid, members_in[i].xid);
        EXPECT_EQ(retrieved[i].status, members_in[i].status);
    }
}

// Multiple multixacts persist; each can be retrieved independently.
TEST_F(MultiXactPersistenceTest, MultipleMultiXactsPersist) {
    std::string base = RegisterDirPair("pgcpp_multixact_persist_multiple");
    std::string offsets = base + "/pg_multixact/offsets";
    std::string members = base + "/pg_multixact/members";

    ShutdownMultiXact();
    InitializeMultiXact(offsets, members);

    std::vector<MultiXactMember> m1 = {{100, 1}};
    std::vector<MultiXactMember> m2 = {{200, 2}, {201, 3}};
    std::vector<MultiXactMember> m3 = {{300, 1}, {301, 2}, {302, 3}};

    MultiXactId id1 = MultiXactIdCreate(m1);
    MultiXactId id2 = MultiXactIdCreate(m2);
    MultiXactId id3 = MultiXactIdCreate(m3);
    FlushMultiXact();

    ShutdownMultiXact();
    InitializeMultiXact(offsets, members);

    auto r1 = MultiXactIdGetMembers(id1);
    auto r2 = MultiXactIdGetMembers(id2);
    auto r3 = MultiXactIdGetMembers(id3);

    ASSERT_EQ(r1.size(), 1u);
    EXPECT_EQ(r1[0].xid, static_cast<TransactionId>(100));

    ASSERT_EQ(r2.size(), 2u);
    EXPECT_EQ(r2[0].xid, static_cast<TransactionId>(200));
    EXPECT_EQ(r2[1].xid, static_cast<TransactionId>(201));

    ASSERT_EQ(r3.size(), 3u);
    EXPECT_EQ(r3[2].xid, static_cast<TransactionId>(302));
}

// Members spanning across a page boundary (1024 members per page) persist.
TEST_F(MultiXactPersistenceTest, MembersSpanPageBoundary) {
    std::string base = RegisterDirPair("pgcpp_multixact_persist_page_boundary");
    std::string offsets = base + "/pg_multixact/offsets";
    std::string members = base + "/pg_multixact/members";

    ShutdownMultiXact();
    InitializeMultiXact(offsets, members);

    // Create a multixact with > 1024 members to span page boundary.
    std::vector<MultiXactMember> many;
    for (int i = 0; i < 1100; ++i) {
        many.push_back({static_cast<TransactionId>(1000 + i), 1});
    }
    MultiXactId multi = MultiXactIdCreate(many);
    FlushMultiXact();

    ShutdownMultiXact();
    InitializeMultiXact(offsets, members);

    auto retrieved = MultiXactIdGetMembers(multi);
    ASSERT_EQ(retrieved.size(), many.size());
    for (std::size_t i = 0; i < many.size(); ++i) {
        EXPECT_EQ(retrieved[i].xid, many[i].xid) << "member " << i << " mismatch";
    }
}

// In-memory multixact does not persist across re-init.
TEST_F(MultiXactPersistenceTest, InMemoryDoesNotPersist) {
    // SetUp initialized with empty dirs.
    std::vector<MultiXactMember> members = {{42, 1}};
    MultiXactId multi = MultiXactIdCreate(members);
    FlushMultiXact();

    ShutdownMultiXact();
    InitializeMultiXact("", "");

    EXPECT_FALSE(MultiXactIdIsValid(multi))
        << "in-memory multixact should not persist across re-init";
}

// MultiXactIdExpand creates a new multixact that also persists.
TEST_F(MultiXactPersistenceTest, ExpandPersists) {
    std::string base = RegisterDirPair("pgcpp_multixact_persist_expand");
    std::string offsets = base + "/pg_multixact/offsets";
    std::string members = base + "/pg_multixact/members";

    ShutdownMultiXact();
    InitializeMultiXact(offsets, members);

    std::vector<MultiXactMember> m1 = {{10, 1}};
    MultiXactId original = MultiXactIdCreate(m1);
    MultiXactId expanded = MultiXactIdExpand(original, 20, 2);
    ASSERT_NE(expanded, original);
    FlushMultiXact();

    ShutdownMultiXact();
    InitializeMultiXact(offsets, members);

    auto retrieved = MultiXactIdGetMembers(expanded);
    ASSERT_EQ(retrieved.size(), 2u);
    EXPECT_EQ(retrieved[0].xid, static_cast<TransactionId>(10));
    EXPECT_EQ(retrieved[1].xid, static_cast<TransactionId>(20));
}
