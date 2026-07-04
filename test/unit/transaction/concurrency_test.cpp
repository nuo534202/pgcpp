// concurrency_test.cpp — Tests for shared-memory backing across fork().
//
// Verifies that ShmemInit + ShmemInitStruct + ShmemAttach work correctly:
// a child process forked after ShmemInit can attach and see writes the
// parent made to a named shared-memory region. This is the foundation of
// pgcpp's multi-process model (A-1 fix).
//
// The fork() test is skipped under ASan/TSan because sanitizers don't
// follow fork() cleanly and report false leaks.

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

#include "storage/ipc/shmem.hpp"

using pgcpp::storage::IsShmemActive;
using pgcpp::storage::ShmemAttach;
using pgcpp::storage::ShmemDetach;
using pgcpp::storage::ShmemInit;
using pgcpp::storage::ShmemInitStruct;

// Detect sanitizers (ASan / TSan) to skip fork-based tests.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PGCPP_HAS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PGCPP_HAS_SANITIZER 1
#endif

namespace {

constexpr std::size_t kShmSize = 1 << 20;  // 1 MB
constexpr char kTestRegion[] = "pgcpp_concurrency_test_region";
constexpr std::size_t kRegionSize = 256;
constexpr uint8_t kSentinel = 0xAB;

class ConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Defensive: ensure no stale segment from a previous test.
        ShmemDetach();
    }
    void TearDown() override { ShmemDetach(); }
};

// ShmemInit creates an active shared-memory segment.
TEST_F(ConcurrencyTest, ShmemInitCreatesActiveSegment) {
    ASSERT_TRUE(ShmemInit(kShmSize));
    EXPECT_TRUE(IsShmemActive());
}

// ShmemInitStruct returns a stable pointer for the same name.
TEST_F(ConcurrencyTest, ShmemInitStructReturnsStableRegion) {
    ASSERT_TRUE(ShmemInit(kShmSize));

    bool found = false;
    void* ptr1 = ShmemInitStruct(kTestRegion, kRegionSize, &found);
    ASSERT_NE(ptr1, nullptr);
    EXPECT_FALSE(found);  // first call → not found

    // Write a sentinel byte.
    static_cast<uint8_t*>(ptr1)[0] = kSentinel;

    // Re-acquire: should be found and return the same pointer.
    bool found2 = false;
    void* ptr2 = ShmemInitStruct(kTestRegion, kRegionSize, &found2);
    EXPECT_EQ(ptr2, ptr1);
    EXPECT_TRUE(found2);  // second call → found
    EXPECT_EQ(static_cast<uint8_t*>(ptr2)[0], kSentinel);
}

// A fork'd child can attach and see writes the parent made to a shm region.
// Skipped under ASan/TSan (fork + sanitizers produce false leak reports).
TEST_F(ConcurrencyTest, ShmemVisibleAcrossFork) {
#ifdef PGCPP_HAS_SANITIZER
    GTEST_SKIP() << "fork() not compatible with sanitizers";
#else
    ASSERT_TRUE(ShmemInit(kShmSize));

    // Allocate a region and write a sentinel.
    bool found = false;
    uint8_t* region = static_cast<uint8_t*>(ShmemInitStruct(kTestRegion, kRegionSize, &found));
    ASSERT_NE(region, nullptr);
    ASSERT_FALSE(found);
    region[0] = kSentinel;

    pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork() failed";

    if (pid == 0) {
        // Child: attach to the inherited shared segment.
        if (!ShmemAttach()) {
            _exit(2);
        }
        bool child_found = false;
        uint8_t* child_region =
            static_cast<uint8_t*>(ShmemInitStruct(kTestRegion, kRegionSize, &child_found));
        if (child_region == nullptr) {
            _exit(3);
        }
        if (!child_found) {
            _exit(4);
        }
        if (child_region[0] != kSentinel) {
            _exit(5);
        }
        _exit(0);
    }

    // Parent: wait for child and check exit status.
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status)) << "child did not exit normally";
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child verification failed";
#endif
}

}  // namespace
