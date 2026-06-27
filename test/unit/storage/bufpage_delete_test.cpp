// bufpage_delete_test.cpp — Unit tests for page deletion/compaction ops (M6 Task 15.7.2).
//
// Tests PageGetFreeSpace, PageIndexTupleDelete, PageIndexMultiDelete,
// PageIndexTupleDeleteNoCompact, PageIndexTupleOverwrite,
// PageRepairFragmentation, and the PageGetTempPage* family. These cover the
// DELETE / UPDATE / VACUUM page-manipulation surface added in Task 15.7.2.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/storage/bufpage.hpp"

using mytoydb::memory::AllocSetContext;
using mytoydb::storage::ItemIdData;
using mytoydb::storage::ItemIdGetLength;
using mytoydb::storage::ItemIdGetOffset;
using mytoydb::storage::ItemIdIsNormal;
using mytoydb::storage::ItemIdIsUsed;
using mytoydb::storage::kBlckSz;
using mytoydb::storage::kInvalidOffsetNumber;
using mytoydb::storage::kPageHasFreeLines;
using mytoydb::storage::kPageHeaderSize;
using mytoydb::storage::LocationIndex;
using mytoydb::storage::OffsetNumber;
using mytoydb::storage::Page;
using mytoydb::storage::PageAddItem;
using mytoydb::storage::PageGetExactFreeSpace;
using mytoydb::storage::PageGetFreeSpace;
using mytoydb::storage::PageGetFreeSpaceForMultipleTuples;
using mytoydb::storage::PageGetItem;
using mytoydb::storage::PageGetItemId;
using mytoydb::storage::PageGetMaxOffsetNumber;
using mytoydb::storage::PageGetTempPageCopy;
using mytoydb::storage::PageHeader;
using mytoydb::storage::PageIndexMultiDelete;
using mytoydb::storage::PageIndexTupleDelete;
using mytoydb::storage::PageIndexTupleDeleteNoCompact;
using mytoydb::storage::PageIndexTupleOverwrite;
using mytoydb::storage::PageInit;
using mytoydb::storage::PageRepairFragmentation;
using mytoydb::storage::PageRestoreTempPage;

namespace {

class BufPageDeleteTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("bufpage_delete_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        page_ = static_cast<char*>(mytoydb::memory::palloc(kBlckSz));
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
    char* page_ = nullptr;
};

}  // namespace

// --- PageGetFreeSpace tests ---

TEST_F(BufPageDeleteTest, PageGetFreeSpace_EmptyPage) {
    PageInit(page_, kBlckSz, 0);
    auto* phdr = reinterpret_cast<PageHeader>(page_);

    int expected = (phdr->pd_upper - phdr->pd_lower) - static_cast<int>(sizeof(ItemIdData));
    // MAXALIGN-down of a value already 4-aligned is a no-op.
    expected &= ~(static_cast<int>(sizeof(ItemIdData)) - 1);

    EXPECT_EQ(PageGetFreeSpace(page_), expected);
    EXPECT_GT(PageGetFreeSpace(page_), 0);
}

TEST_F(BufPageDeleteTest, PageGetFreeSpace_AfterAddItem) {
    PageInit(page_, kBlckSz, 0);
    int before = PageGetFreeSpace(page_);

    char item[] = "some test data for the page";
    OffsetNumber off = PageAddItem(page_, item, sizeof(item), kInvalidOffsetNumber, true);
    ASSERT_EQ(off, 1);

    int after = PageGetFreeSpace(page_);
    EXPECT_LT(after, before);
    // Free space should drop by the aligned item size plus one line pointer.
    int aligned_size = (static_cast<int>(sizeof(item)) + 3) & ~3;
    EXPECT_LE(before - after, aligned_size + static_cast<int>(sizeof(ItemIdData)));
}

TEST_F(BufPageDeleteTest, PageGetExactFreeSpaceAndMultipleTuples) {
    PageInit(page_, kBlckSz, 0);
    auto* phdr = reinterpret_cast<PageHeader>(page_);

    int exact = (phdr->pd_upper - phdr->pd_lower);
    EXPECT_EQ(PageGetExactFreeSpace(page_), exact);

    // Free space for 3 tuples must be smaller than the single-tuple variant.
    // Going from n=1 to n=3 adds 2 line pointers; since sizeof(ItemIdData)
    // equals the alignment (4), MAXALIGN-down never absorbs the difference,
    // so the drop is exactly 2 * sizeof(ItemIdData).
    int one = PageGetFreeSpace(page_);
    int three = PageGetFreeSpaceForMultipleTuples(page_, 3);
    EXPECT_LE(three, one);
    EXPECT_EQ(one - three, 2 * static_cast<int>(sizeof(ItemIdData)));
}

// --- PageIndexTupleDelete tests ---

TEST_F(BufPageDeleteTest, PageIndexTupleDelete_RemovesItem) {
    PageInit(page_, kBlckSz, 0);
    char item[] = "tuple to delete";
    OffsetNumber off = PageAddItem(page_, item, sizeof(item), kInvalidOffsetNumber, true);
    ASSERT_EQ(off, 1);

    PageIndexTupleDelete(page_, 1);

    auto* lp = PageGetItemId(page_, 1);
    EXPECT_FALSE(ItemIdIsUsed(lp));
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 0);
}

TEST_F(BufPageDeleteTest, PageIndexTupleDelete_CompactsPage) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "AAAA";
    char b[] = "BBBBBBBB";  // longer, sits at a lower offset
    char c[] = "CC";
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);
    ASSERT_EQ(PageAddItem(page_, c, sizeof(c), kInvalidOffsetNumber, true), 3);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    int upper_before = phdr->pd_upper;

    // Delete the middle tuple; the two survivors must remain readable and the
    // page upper bound must advance by the deleted (aligned) size.
    PageIndexTupleDelete(page_, 2);

    int upper_after = phdr->pd_upper;
    EXPECT_GT(upper_after, upper_before);

    // Items 1 and 3 are still normal and their data is intact.
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 1)));
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 3)));
    EXPECT_EQ(std::memcmp(PageGetItem(page_, PageGetItemId(page_, 1)), a, sizeof(a)), 0);
    EXPECT_EQ(std::memcmp(PageGetItem(page_, PageGetItemId(page_, 3)), c, sizeof(c)), 0);

    // Item 2 is now unused.
    EXPECT_FALSE(ItemIdIsUsed(PageGetItemId(page_, 2)));
}

TEST_F(BufPageDeleteTest, PageIndexTupleDelete_LastShrinksPdLower) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "alpha";
    char b[] = "beta";
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    int lower_before = phdr->pd_lower;

    PageIndexTupleDelete(page_, 2);

    // The trailing line pointer was reclaimed: pd_lower drops by one ItemIdData.
    EXPECT_EQ(phdr->pd_lower, lower_before - static_cast<int>(sizeof(ItemIdData)));
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 1);
}

// --- PageIndexMultiDelete tests ---

TEST_F(BufPageDeleteTest, PageIndexMultiDelete_RemovesMultiple) {
    PageInit(page_, kBlckSz, 0);
    for (int i = 0; i < 5; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "t%d", i);
        ASSERT_EQ(PageAddItem(page_, buf, std::strlen(buf) + 1, kInvalidOffsetNumber, true),
                  static_cast<OffsetNumber>(i + 1));
    }

    OffsetNumber to_delete[] = {1, 3, 5};
    PageIndexMultiDelete(page_, to_delete, 3);

    // Survivors: offsets 2 and 4 remain normal; the rest are unused.
    EXPECT_FALSE(ItemIdIsUsed(PageGetItemId(page_, 1)));
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 2)));
    EXPECT_FALSE(ItemIdIsUsed(PageGetItemId(page_, 3)));
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 4)));
    EXPECT_FALSE(ItemIdIsUsed(PageGetItemId(page_, 5)));

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    EXPECT_TRUE(phdr->pd_flags & kPageHasFreeLines);
}

TEST_F(BufPageDeleteTest, PageIndexMultiDelete_CompactsTupleData) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "AAAAAAAA";  // 9 bytes raw, 12 aligned
    char b[] = "BBBB";
    char c[] = "CCCCCCCC";  // 9 bytes raw, 12 aligned
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);
    ASSERT_EQ(PageAddItem(page_, c, sizeof(c), kInvalidOffsetNumber, true), 3);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    int upper_before = phdr->pd_upper;

    OffsetNumber to_delete[] = {2};
    PageIndexMultiDelete(page_, to_delete, 1);

    EXPECT_GT(phdr->pd_upper, upper_before);
    // Survivors are still readable.
    EXPECT_EQ(std::memcmp(PageGetItem(page_, PageGetItemId(page_, 1)), a, sizeof(a)), 0);
    EXPECT_EQ(std::memcmp(PageGetItem(page_, PageGetItemId(page_, 3)), c, sizeof(c)), 0);
}

// --- PageIndexTupleDeleteNoCompact tests ---

TEST_F(BufPageDeleteTest, PageIndexTupleDeleteNoCompact_MarksUnused) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "alpha";
    char b[] = "beta";
    char c[] = "gamma";
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);
    ASSERT_EQ(PageAddItem(page_, c, sizeof(c), kInvalidOffsetNumber, true), 3);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    int upper_before = phdr->pd_upper;

    PageIndexTupleDeleteNoCompact(page_, 2);

    // No compaction: pd_upper must not move.
    EXPECT_EQ(phdr->pd_upper, upper_before);
    // Middle hole => P_HAS_FREE_LINES set.
    EXPECT_TRUE(phdr->pd_flags & kPageHasFreeLines);
    // Item 2 unused, items 1 and 3 still normal.
    EXPECT_FALSE(ItemIdIsUsed(PageGetItemId(page_, 2)));
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 1)));
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 3)));
}

TEST_F(BufPageDeleteTest, PageIndexTupleDeleteNoCompact_LastShrinksPdLower) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "alpha";
    char b[] = "beta";
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    int lower_before = phdr->pd_lower;

    // Deleting the last entry must shrink pd_lower and NOT set P_HAS_FREE_LINES
    // (no middle hole is created).
    PageIndexTupleDeleteNoCompact(page_, 2);

    EXPECT_EQ(phdr->pd_lower, lower_before - static_cast<int>(sizeof(ItemIdData)));
    EXPECT_FALSE(phdr->pd_flags & kPageHasFreeLines);
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 1);
}

// --- PageRepairFragmentation tests ---

TEST_F(BufPageDeleteTest, PageRepairFragmentation_RemovesDeadItems) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "alpha";
    char b[] = "beta";
    char c[] = "gamma";
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);
    ASSERT_EQ(PageAddItem(page_, c, sizeof(c), kInvalidOffsetNumber, true), 3);

    // Kill the middle item without compacting, leaving a hole in the line
    // pointer array and a gap in the tuple area.
    PageIndexTupleDeleteNoCompact(page_, 2);

    PageRepairFragmentation(page_);

    // After repair: only two normal items, packed at the front of the array.
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 2);
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 1)));
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 2)));
    // P_HAS_FREE_LINES cleared (no holes remain).
    auto* phdr = reinterpret_cast<PageHeader>(page_);
    EXPECT_FALSE(phdr->pd_flags & kPageHasFreeLines);

    // Item data is preserved (values 'alpha' and 'gamma' survive in some
    // order; check by content rather than offset number).
    char* v1 = PageGetItem(page_, PageGetItemId(page_, 1));
    char* v2 = PageGetItem(page_, PageGetItemId(page_, 2));
    bool has_alpha = std::memcmp(v1, a, sizeof(a)) == 0 || std::memcmp(v2, a, sizeof(a)) == 0;
    bool has_gamma = std::memcmp(v1, c, sizeof(c)) == 0 || std::memcmp(v2, c, sizeof(c)) == 0;
    EXPECT_TRUE(has_alpha);
    EXPECT_TRUE(has_gamma);
}

TEST_F(BufPageDeleteTest, PageRepairFragmentation_CompactsFreeSpace) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "AAAAAAAA";  // 9 raw, 12 aligned
    char b[] = "BBBB";      // 5 raw, 8 aligned
    char c[] = "CCCCCCCC";  // 9 raw, 12 aligned
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);
    ASSERT_EQ(PageAddItem(page_, c, sizeof(c), kInvalidOffsetNumber, true), 3);

    // Delete item 2 without compacting, leaving a gap of 8 bytes.
    PageIndexTupleDeleteNoCompact(page_, 2);

    int free_before = PageGetExactFreeSpace(page_);

    PageRepairFragmentation(page_);

    int free_after = PageGetExactFreeSpace(page_);
    // The 8-byte gap left by item 2 is reclaimed into free space.
    EXPECT_GT(free_after, free_before);
    EXPECT_GE(free_after - free_before, 8);
}

TEST_F(BufPageDeleteTest, PageRepairFragmentation_AllDeadResetsPage) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "alpha";
    char b[] = "beta";
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);

    PageIndexTupleDeleteNoCompact(page_, 1);
    PageIndexTupleDeleteNoCompact(page_, 2);

    PageRepairFragmentation(page_);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    EXPECT_EQ(phdr->pd_lower, kPageHeaderSize);
    EXPECT_EQ(phdr->pd_upper, phdr->pd_special);
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 0);
}

// --- PageIndexTupleOverwrite tests ---

TEST_F(BufPageDeleteTest, PageIndexTupleOverwrite_SameSize) {
    PageInit(page_, kBlckSz, 0);
    char original[] = "original";
    ASSERT_EQ(PageAddItem(page_, original, sizeof(original), kInvalidOffsetNumber, true), 1);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    int upper_before = phdr->pd_upper;
    LocationIndex off_before = ItemIdGetOffset(PageGetItemId(page_, 1));

    char replacement[] = "REPLACED";  // same length as "original"
    ASSERT_EQ(sizeof(replacement), sizeof(original));
    bool ok = PageIndexTupleOverwrite(page_, 1, replacement, sizeof(replacement));

    EXPECT_TRUE(ok);
    // Offset and upper bound unchanged for same-size overwrite.
    EXPECT_EQ(ItemIdGetOffset(PageGetItemId(page_, 1)), off_before);
    EXPECT_EQ(phdr->pd_upper, upper_before);
    EXPECT_EQ(
        std::memcmp(PageGetItem(page_, PageGetItemId(page_, 1)), replacement, sizeof(replacement)),
        0);
}

TEST_F(BufPageDeleteTest, PageIndexTupleOverwrite_SmallerSize) {
    PageInit(page_, kBlckSz, 0);
    char original[] = "ORIGINAL_LONG";  // 14 raw, 16 aligned
    ASSERT_EQ(PageAddItem(page_, original, sizeof(original), kInvalidOffsetNumber, true), 1);

    auto* phdr = reinterpret_cast<PageHeader>(page_);
    int upper_before = phdr->pd_upper;

    char replacement[] = "SHORT";  // 6 raw, 8 aligned — smaller than original
    bool ok = PageIndexTupleOverwrite(page_, 1, replacement, sizeof(replacement));

    EXPECT_TRUE(ok);
    EXPECT_EQ(ItemIdGetLength(PageGetItemId(page_, 1)), sizeof(replacement));
    EXPECT_EQ(
        std::memcmp(PageGetItem(page_, PageGetItemId(page_, 1)), replacement, sizeof(replacement)),
        0);
    // Compaction closed the gap: pd_upper advanced.
    EXPECT_GT(phdr->pd_upper, upper_before);
}

TEST_F(BufPageDeleteTest, PageIndexTupleOverwrite_LargerFails) {
    PageInit(page_, kBlckSz, 0);
    char original[] = "tiny";  // 4 raw, 4 aligned
    ASSERT_EQ(PageAddItem(page_, original, sizeof(original), kInvalidOffsetNumber, true), 1);

    LocationIndex off_before = ItemIdGetOffset(PageGetItemId(page_, 1));
    uint16_t len_before = ItemIdGetLength(PageGetItemId(page_, 1));

    char replacement[] = "this is much longer than the original";  // > sizeof(original)
    ASSERT_GT(sizeof(replacement), sizeof(original));
    bool ok = PageIndexTupleOverwrite(page_, 1, replacement, sizeof(replacement));

    EXPECT_FALSE(ok);
    // On failure the line pointer is left untouched.
    EXPECT_EQ(ItemIdGetOffset(PageGetItemId(page_, 1)), off_before);
    EXPECT_EQ(ItemIdGetLength(PageGetItemId(page_, 1)), len_before);
    EXPECT_EQ(std::memcmp(PageGetItem(page_, PageGetItemId(page_, 1)), original, sizeof(original)),
              0);
}

// --- PageGetTempPageCopy / PageRestoreTempPage tests ---

TEST_F(BufPageDeleteTest, PageGetTempPageCopy_RoundTrip) {
    PageInit(page_, kBlckSz, 0);
    char a[] = "first item";
    char b[] = "second item";
    ASSERT_EQ(PageAddItem(page_, a, sizeof(a), kInvalidOffsetNumber, true), 1);
    ASSERT_EQ(PageAddItem(page_, b, sizeof(b), kInvalidOffsetNumber, true), 2);

    int free_before = PageGetExactFreeSpace(page_);

    // Take a full copy, mutate it, then restore it back.
    Page temp = PageGetTempPageCopy(page_);
    ASSERT_NE(temp, nullptr);

    // The copy must be byte-identical.
    EXPECT_EQ(std::memcmp(temp, page_, kBlckSz), 0);

    // Mutate the copy: append a third item.
    char c[] = "third item";
    OffsetNumber off = PageAddItem(temp, c, sizeof(c), kInvalidOffsetNumber, true);
    EXPECT_EQ(off, 3);
    EXPECT_LT(PageGetExactFreeSpace(temp), free_before);

    // Original is untouched until restore.
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 2);

    PageRestoreTempPage(temp, page_);

    // After restore, the original reflects the copy's state.
    EXPECT_EQ(PageGetMaxOffsetNumber(page_), 3);
    EXPECT_TRUE(ItemIdIsNormal(PageGetItemId(page_, 3)));
    EXPECT_EQ(std::memcmp(PageGetItem(page_, PageGetItemId(page_, 3)), c, sizeof(c)), 0);
    // Earlier items survive the round-trip.
    EXPECT_EQ(std::memcmp(PageGetItem(page_, PageGetItemId(page_, 1)), a, sizeof(a)), 0);
    EXPECT_EQ(std::memcmp(PageGetItem(page_, PageGetItemId(page_, 2)), b, sizeof(b)), 0);
}
