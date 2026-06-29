// bufpage_test.cpp — Unit tests for page operations (M6 Task 6.1/6.2).
//
// Tests PageInit, PageAddItem, PageGetItem, and related page macros.
// Verifies that the page layout matches PostgreSQL's design: header,
// line pointer array, free space, and tuple data.

#include "storage/bufpage.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

using pgcpp::memory::AllocSetContext;
using pgcpp::storage::BlockNumber;
using pgcpp::storage::ItemIdData;
using pgcpp::storage::ItemIdGetFlags;
using pgcpp::storage::ItemIdGetLength;
using pgcpp::storage::ItemIdGetOffset;
using pgcpp::storage::ItemIdIsNormal;
using pgcpp::storage::ItemIdIsUsed;
using pgcpp::storage::kBlckSz;
using pgcpp::storage::kInvalidOffsetNumber;
using pgcpp::storage::kLPNormal;
using pgcpp::storage::kLPUnused;
using pgcpp::storage::kPageHeaderSize;
using pgcpp::storage::kPageSizeVersion;
using pgcpp::storage::OffsetNumber;
using pgcpp::storage::Page;
using pgcpp::storage::PageAddItem;
using pgcpp::storage::PageGetContents;
using pgcpp::storage::PageGetHeapFreeSpace;
using pgcpp::storage::PageGetItem;
using pgcpp::storage::PageGetItemId;
using pgcpp::storage::PageGetMaxOffsetNumber;
using pgcpp::storage::PageGetSpecialPointer;
using pgcpp::storage::PageHeader;
using pgcpp::storage::PageHeaderData;
using pgcpp::storage::PageInit;

namespace {

class BufPageTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("bufpage_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        // Allocate a page.
        page_ = static_cast<char*>(pgcpp::memory::palloc(kBlckSz));
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
    char* page_ = nullptr;
};

}  // namespace

// --- PageInit tests ---

TEST_F(BufPageTest, PageInitSetsHeaderFields) {
    PageInit(page_, kBlckSz, 0);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    EXPECT_EQ(phdr->pd_lsn, 0);
    EXPECT_EQ(phdr->pd_checksum, 0);
    EXPECT_EQ(phdr->pd_flags, 0);
    EXPECT_EQ(phdr->pd_lower, kPageHeaderSize);
    EXPECT_EQ(phdr->pd_upper, kBlckSz);
    EXPECT_EQ(phdr->pd_special, kBlckSz);
    EXPECT_EQ(phdr->pd_pagesize_version, kPageSizeVersion);
    EXPECT_EQ(phdr->pd_prune_xid, 0);
}

TEST_F(BufPageTest, PageInitWithSpecialArea) {
    int special_size = 128;
    PageInit(page_, kBlckSz, special_size);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    EXPECT_EQ(phdr->pd_lower, kPageHeaderSize);
    EXPECT_EQ(phdr->pd_upper, kBlckSz - special_size);
    EXPECT_EQ(phdr->pd_special, kBlckSz - special_size);
}

TEST_F(BufPageTest, PageInitClearsPage) {
    // Fill the page with non-zero data first.
    std::memset(page_, 0xFF, kBlckSz);

    // PageInit should zero everything.
    PageInit(page_, kBlckSz, 0);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    EXPECT_EQ(phdr->pd_lower, kPageHeaderSize);

    // Check that the content area is zeroed.
    for (int i = kPageHeaderSize; i < kBlckSz; ++i) {
        EXPECT_EQ(page_[i], 0) << "byte " << i << " not zeroed";
    }
}

// --- PageAddItem tests ---

TEST_F(BufPageTest, PageAddItemFirstItem) {
    PageInit(page_, kBlckSz, 0);

    char item_data[] = "hello world";
    OffsetNumber offset =
        PageAddItem(page_, item_data, sizeof(item_data), kInvalidOffsetNumber, true);

    EXPECT_EQ(offset, 1);
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 1);

    auto* item_id = PageGetItemId(page_, 1);
    EXPECT_TRUE(ItemIdIsUsed(item_id));
    EXPECT_TRUE(ItemIdIsNormal(item_id));
    EXPECT_EQ(ItemIdGetFlags(item_id), kLPNormal);
    EXPECT_EQ(ItemIdGetLength(item_id), sizeof(item_data));
    EXPECT_GT(ItemIdGetOffset(item_id), 0);

    // Verify the item data can be retrieved.
    char* item = PageGetItem(page_, item_id);
    EXPECT_STREQ(item, "hello world");
}

TEST_F(BufPageTest, PageAddItemMultipleItems) {
    PageInit(page_, kBlckSz, 0);

    char item1[] = "first";
    char item2[] = "second";
    char item3[] = "third";

    OffsetNumber off1 = PageAddItem(page_, item1, sizeof(item1), kInvalidOffsetNumber, true);
    OffsetNumber off2 = PageAddItem(page_, item2, sizeof(item2), kInvalidOffsetNumber, true);
    OffsetNumber off3 = PageAddItem(page_, item3, sizeof(item3), kInvalidOffsetNumber, true);

    EXPECT_EQ(off1, 1);
    EXPECT_EQ(off2, 2);
    EXPECT_EQ(off3, 3);
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 3);

    // Verify all items are retrievable.
    EXPECT_STREQ(PageGetItem(page_, PageGetItemId(page_, 1)), "first");
    EXPECT_STREQ(PageGetItem(page_, PageGetItemId(page_, 2)), "second");
    EXPECT_STREQ(PageGetItem(page_, PageGetItemId(page_, 3)), "third");
}

TEST_F(BufPageTest, PageAddItemUpdatesFreeSpace) {
    PageInit(page_, kBlckSz, 0);

    int initial_free = PageGetHeapFreeSpace(page_);
    EXPECT_GT(initial_free, 0);

    char item[] = "some data";
    PageAddItem(page_, item, sizeof(item), kInvalidOffsetNumber, true);

    int after_free = PageGetHeapFreeSpace(page_);
    EXPECT_LT(after_free, initial_free);
}

TEST_F(BufPageTest, PageAddItemFailsWhenFull) {
    PageInit(page_, kBlckSz, 0);

    // Fill the page with large items until it's full.
    char big_item[4000];
    std::memset(big_item, 'X', sizeof(big_item));

    int count = 0;
    while (true) {
        OffsetNumber off =
            PageAddItem(page_, big_item, sizeof(big_item), kInvalidOffsetNumber, true);
        if (off == kInvalidOffsetNumber)
            break;
        ++count;
    }

    // Should have added at least 1 item.
    EXPECT_GE(count, 1);
    // The next add should fail.
    EXPECT_EQ(PageAddItem(page_, big_item, sizeof(big_item), kInvalidOffsetNumber, true),
              kInvalidOffsetNumber);
}

// --- Page macro tests ---

TEST_F(BufPageTest, PageGetMaxOffsetNumberOnEmptyPage) {
    PageInit(page_, kBlckSz, 0);
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 0);
}

TEST_F(BufPageTest, PageGetSpecialPointer) {
    int special_size = 64;
    PageInit(page_, kBlckSz, special_size);

    char* special = PageGetSpecialPointer(page_);
    EXPECT_EQ(special, page_ + kBlckSz - special_size);
}

TEST_F(BufPageTest, PageGetContents) {
    PageInit(page_, kBlckSz, 0);

    char* contents = PageGetContents(page_);
    EXPECT_EQ(contents, page_ + kPageHeaderSize);
}

TEST_F(BufPageTest, ItemIdFlags) {
    PageInit(page_, kBlckSz, 0);

    char item[] = "test";
    PageAddItem(page_, item, sizeof(item), kInvalidOffsetNumber, true);

    auto* item_id = PageGetItemId(page_, 1);
    EXPECT_TRUE(ItemIdIsUsed(item_id));
    EXPECT_TRUE(ItemIdIsNormal(item_id));

    // An unused line pointer (offset 2, which doesn't exist yet).
    // We can't directly access it, but we can verify the first one.
    EXPECT_EQ(ItemIdGetFlags(item_id), kLPNormal);
    EXPECT_EQ(ItemIdGetFlags(item_id), kLPUnused + 1);
}
