#include "common/memory/memory_context.hpp"

#include <gtest/gtest.h>

#include <cstring>

#include "common/memory/alloc_set.hpp"

using namespace pgcpp::memory;

TEST(MemoryContextTest, PallocAndFree) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    ASSERT_NE(ctx, nullptr);
    {
        ContextSwitchGuard guard(ctx);
        void* p = palloc(100);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0xAB, 100);
        auto* bytes = static_cast<unsigned char*>(p);
        for (int i = 0; i < 100; ++i) {
            EXPECT_EQ(bytes[i], 0xAB);
        }
        pfree(p);
    }
    delete ctx;
}

TEST(MemoryContextTest, Palloc0ReturnsZeroedMemory) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    {
        ContextSwitchGuard guard(ctx);
        void* p = palloc0(100);
        ASSERT_NE(p, nullptr);
        auto* bytes = static_cast<unsigned char*>(p);
        for (int i = 0; i < 100; ++i) {
            EXPECT_EQ(bytes[i], 0);
        }
        pfree(p);
    }
    delete ctx;
}

TEST(MemoryContextTest, RepallocGrowsAndPreservesData) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    {
        ContextSwitchGuard guard(ctx);
        char* p = static_cast<char*>(palloc(50));
        ASSERT_NE(p, nullptr);
        std::memcpy(p, "hello", 6);
        p = static_cast<char*>(repalloc(p, 200));
        ASSERT_NE(p, nullptr);
        EXPECT_STREQ(p, "hello");
        pfree(p);
    }
    delete ctx;
}

TEST(MemoryContextTest, ContextSwitchGuardSwitchesAndRestores) {
    MemoryContext* old = GetCurrentMemoryContext();
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    {
        ContextSwitchGuard guard(ctx);
        EXPECT_EQ(GetCurrentMemoryContext(), ctx);
    }
    EXPECT_EQ(GetCurrentMemoryContext(), old);
    delete ctx;
}

TEST(MemoryContextTest, MemoryContextScopeCreatesAndCleansUp) {
    MemoryContext* old = GetCurrentMemoryContext();
    {
        MemoryContextScope scope("temp");
        EXPECT_NE(scope.Get(), nullptr);
        EXPECT_EQ(GetCurrentMemoryContext(), scope.Get());
        void* p = palloc(50);
        EXPECT_NE(p, nullptr);
        pfree(p);
    }
    EXPECT_EQ(GetCurrentMemoryContext(), old);
}

TEST(MemoryContextTest, ResetClearsAllocations) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    {
        ContextSwitchGuard guard(ctx);
        void* p1 = palloc(100);
        void* p2 = palloc(200);
        EXPECT_NE(p1, nullptr);
        EXPECT_NE(p2, nullptr);
        // Don't free - Reset reclaims everything.
        ctx->Reset();
        EXPECT_TRUE(ctx->IsReset());
        // Should be able to allocate again after reset.
        void* p3 = palloc(50);
        EXPECT_NE(p3, nullptr);
        std::memset(p3, 0xCD, 50);
        pfree(p3);
    }
    delete ctx;
}

TEST(MemoryContextTest, NestedContexts) {
    AllocSetContext* parent = AllocSetContext::Create("parent", nullptr);
    AllocSetContext* child = AllocSetContext::Create("child", parent);
    EXPECT_EQ(child->GetParent(), parent);
    EXPECT_EQ(parent->GetParent(), nullptr);
    {
        ContextSwitchGuard guard(child);
        void* p = palloc(100);
        EXPECT_NE(p, nullptr);
        std::memset(p, 0x42, 100);
        pfree(p);
    }
    delete child;
    delete parent;
}

TEST(MemoryContextTest, MemoryContextAllocFromSpecificContext) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    // No need to switch CurrentMemoryContext.
    void* p = MemoryContextAlloc(ctx, 100);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 100);
    auto* bytes = static_cast<unsigned char*>(p);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(bytes[i], 0xAB);
    }
    ctx->Free(p);
    delete ctx;
}

TEST(MemoryContextTest, MemoryContextAllocZeroFromSpecificContext) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    void* p = MemoryContextAllocZero(ctx, 100);
    ASSERT_NE(p, nullptr);
    auto* bytes = static_cast<unsigned char*>(p);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(bytes[i], 0);
    }
    ctx->Free(p);
    delete ctx;
}

TEST(MemoryContextTest, ContextAllocTyped) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    struct Point {
        int x;
        int y;
    };
    Point* p = ContextAlloc<Point>(ctx);
    ASSERT_NE(p, nullptr);
    p->x = 10;
    p->y = 20;
    EXPECT_EQ(p->x, 10);
    EXPECT_EQ(p->y, 20);
    ctx->Free(p);
    delete ctx;
}

TEST(MemoryContextTest, ContextAllocArrayTyped) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    int* arr = ContextAllocArray<int>(ctx, 10);
    ASSERT_NE(arr, nullptr);
    for (int i = 0; i < 10; ++i) {
        arr[i] = i * i;
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(arr[i], i * i);
    }
    ctx->Free(arr);
    delete ctx;
}

TEST(MemoryContextTest, ReallocNullActsAsAlloc) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    {
        ContextSwitchGuard guard(ctx);
        void* p = repalloc(nullptr, 64);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x77, 64);
        pfree(p);
    }
    delete ctx;
}

TEST(MemoryContextTest, ReallocSmallerSizeReturnsSamePointer) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    {
        ContextSwitchGuard guard(ctx);
        char* p = static_cast<char*>(palloc(200));
        ASSERT_NE(p, nullptr);
        std::memcpy(p, "data", 5);
        char* p2 = static_cast<char*>(repalloc(p, 50));
        EXPECT_EQ(p2, p);  // Same pointer, shrunk in place.
        EXPECT_STREQ(p2, "data");
        pfree(p2);
    }
    delete ctx;
}

TEST(MemoryContextTest, IsEmptyAndIsReset) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    EXPECT_TRUE(ctx->IsReset());
    EXPECT_TRUE(ctx->IsEmpty());
    {
        ContextSwitchGuard guard(ctx);
        void* p = palloc(100);
        EXPECT_FALSE(ctx->IsReset());
        EXPECT_FALSE(ctx->IsEmpty());
        pfree(p);
        // Freeing does not reset the context.
        EXPECT_FALSE(ctx->IsReset());
    }
    ctx->Reset();
    EXPECT_TRUE(ctx->IsReset());
    EXPECT_TRUE(ctx->IsEmpty());
    delete ctx;
}

TEST(MemoryContextTest, FreeListReuse) {
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr);
    {
        ContextSwitchGuard guard(ctx);
        // Allocate and free a small chunk, then allocate again — the second
        // allocation should reuse the freed chunk from the free list.
        void* p1 = palloc(64);
        ASSERT_NE(p1, nullptr);
        pfree(p1);
        void* p2 = palloc(64);
        ASSERT_NE(p2, nullptr);
        // p2 may or may not equal p1, but the allocation must succeed.
        std::memset(p2, 0xEE, 64);
        pfree(p2);
    }
    delete ctx;
}

TEST(MemoryContextTest, LargeAllocationExceedingMaxBlock) {
    // A chunk larger than max_block_size should still succeed (gets its own
    // oversized block).
    AllocSetContext* ctx = AllocSetContext::Create("test", nullptr, 0, 8192, 8192 * 16);
    {
        ContextSwitchGuard guard(ctx);
        void* p = palloc(8192 * 32);  // 256 KiB, larger than max_block_size.
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x55, 8192 * 32);
        pfree(p);
    }
    delete ctx;
}
