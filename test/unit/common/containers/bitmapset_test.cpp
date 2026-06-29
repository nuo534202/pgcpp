#include "common/containers/bitmapset.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace {

using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::Bitmapset;
using pgcpp::nodes::bms_add_member;
using pgcpp::nodes::bms_copy;
using pgcpp::nodes::bms_del_member;
using pgcpp::nodes::bms_difference;
using pgcpp::nodes::bms_equal;
using pgcpp::nodes::bms_free;
using pgcpp::nodes::bms_intersect;
using pgcpp::nodes::bms_is_empty;
using pgcpp::nodes::bms_is_member;
using pgcpp::nodes::bms_is_subset;
using pgcpp::nodes::bms_make_singleton;
using pgcpp::nodes::bms_minimum_member;
using pgcpp::nodes::bms_next_member;
using pgcpp::nodes::bms_nonempty_difference;
using pgcpp::nodes::bms_num_members;
using pgcpp::nodes::bms_overlap;
using pgcpp::nodes::bms_union;

// Bitmapset does NOT use palloc (it is allocated via global new), so the
// fixture exists only to keep parity with the rest of the test suite and to
// host any future palloc'd dependencies. The fixture also ensures that if a
// test ever accidentally calls into palloc, the allocation lands in a
// controlled context rather than crashing on a null CurrentMemoryContext.
class BitmapsetTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = AllocSetContext::Create("bitmapset_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

// --- MakeSingleton + IsMember ----------------------------------------------

TEST_F(BitmapsetTest, MakeSingletonContainsMember) {
    std::unique_ptr<Bitmapset> bms(bms_make_singleton(5));
    ASSERT_NE(bms, nullptr);
    EXPECT_TRUE(bms_is_member(5, bms.get()));
    EXPECT_FALSE(bms_is_member(4, bms.get()));
    EXPECT_FALSE(bms_is_member(6, bms.get()));
}

TEST_F(BitmapsetTest, MakeSingletonZero) {
    std::unique_ptr<Bitmapset> bms(bms_make_singleton(0));
    EXPECT_TRUE(bms_is_member(0, bms.get()));
    EXPECT_FALSE(bms_is_member(1, bms.get()));
}

TEST_F(BitmapsetTest, MakeSingletonLarge) {
    std::unique_ptr<Bitmapset> bms(bms_make_singleton(1000));
    EXPECT_TRUE(bms_is_member(1000, bms.get()));
    EXPECT_FALSE(bms_is_member(999, bms.get()));
}

// --- AddMember + DelMember -------------------------------------------------

TEST_F(BitmapsetTest, AddMemberAccumulates) {
    Bitmapset bms;
    bms.AddMember(1).AddMember(3).AddMember(5);
    EXPECT_EQ(bms.NumMembers(), 3);
    EXPECT_TRUE(bms.IsMember(1));
    EXPECT_TRUE(bms.IsMember(3));
    EXPECT_TRUE(bms.IsMember(5));
    EXPECT_FALSE(bms.IsMember(2));
}

TEST_F(BitmapsetTest, AddMemberIdempotent) {
    Bitmapset bms;
    bms.AddMember(2).AddMember(2).AddMember(2);
    EXPECT_EQ(bms.NumMembers(), 1);
}

TEST_F(BitmapsetTest, DelMemberRemoves) {
    Bitmapset bms;
    bms.AddMember(1).AddMember(2).AddMember(3);
    bms.DelMember(2);
    EXPECT_EQ(bms.NumMembers(), 2);
    EXPECT_FALSE(bms.IsMember(2));
    EXPECT_TRUE(bms.IsMember(1));
    EXPECT_TRUE(bms.IsMember(3));
}

TEST_F(BitmapsetTest, DelMissingMemberNoop) {
    Bitmapset bms;
    bms.AddMember(1);
    bms.DelMember(99);  // not present
    EXPECT_EQ(bms.NumMembers(), 1);
    EXPECT_TRUE(bms.IsMember(1));
}

TEST_F(BitmapsetTest, AddMemberViaFreeFunctionOnNullptr) {
    Bitmapset* bms = bms_add_member(nullptr, 7);
    ASSERT_NE(bms, nullptr);
    EXPECT_TRUE(bms_is_member(7, bms));
    bms_free(bms);
}

// --- IsEmpty / NumMembers / MinMember / MaxMember --------------------------

TEST_F(BitmapsetTest, DefaultIsEmpty) {
    Bitmapset bms;
    EXPECT_TRUE(bms.IsEmpty());
    EXPECT_EQ(bms.NumMembers(), 0);
    EXPECT_EQ(bms.MinMember(), -1);
    EXPECT_EQ(bms.MaxMember(), -1);
}

TEST_F(BitmapsetTest, MinMaxMember) {
    Bitmapset bms;
    bms.AddMember(10).AddMember(3).AddMember(100);
    EXPECT_EQ(bms.MinMember(), 3);
    EXPECT_EQ(bms.MaxMember(), 100);
}

TEST_F(BitmapsetTest, MinMemberAfterDel) {
    Bitmapset bms;
    bms.AddMember(1).AddMember(2).AddMember(3);
    bms.DelMember(1);
    EXPECT_EQ(bms.MinMember(), 2);
}

TEST_F(BitmapsetTest, BmsMinimumMemberNullptr) {
    EXPECT_EQ(bms_minimum_member(nullptr), -1);
}

TEST_F(BitmapsetTest, BmsIsEmptyNullptr) {
    EXPECT_TRUE(bms_is_empty(nullptr));
    EXPECT_EQ(bms_num_members(nullptr), 0);
}

// --- Equals ----------------------------------------------------------------

TEST_F(BitmapsetTest, EqualsSameMembers) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2).AddMember(3);
    b.AddMember(3).AddMember(2).AddMember(1);  // different order
    EXPECT_TRUE(a.Equals(b));
    EXPECT_TRUE(bms_equal(&a, &b));
}

TEST_F(BitmapsetTest, EqualsDifferentMembers) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2);
    b.AddMember(1).AddMember(3);
    EXPECT_FALSE(a.Equals(b));
}

TEST_F(BitmapsetTest, BmsEqualBothNullptr) {
    EXPECT_TRUE(bms_equal(nullptr, nullptr));
}

TEST_F(BitmapsetTest, BmsEqualOneNullptrOneEmpty) {
    Bitmapset empty;
    EXPECT_TRUE(bms_equal(nullptr, &empty));
    EXPECT_TRUE(bms_equal(&empty, nullptr));
}

// --- Overlap / IsSubset / NonemptyDifference -------------------------------

TEST_F(BitmapsetTest, OverlapTrue) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2);
    b.AddMember(2).AddMember(3);
    EXPECT_TRUE(a.Overlap(b));
    EXPECT_TRUE(bms_overlap(&a, &b));
}

TEST_F(BitmapsetTest, OverlapFalse) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2);
    b.AddMember(3).AddMember(4);
    EXPECT_FALSE(a.Overlap(b));
    EXPECT_FALSE(bms_overlap(&a, &b));
}

TEST_F(BitmapsetTest, OverlapWithNullptr) {
    Bitmapset a;
    a.AddMember(1);
    EXPECT_FALSE(bms_overlap(&a, nullptr));
    EXPECT_FALSE(bms_overlap(nullptr, &a));
}

TEST_F(BitmapsetTest, IsSubsetTrue) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2);
    b.AddMember(1).AddMember(2).AddMember(3);
    EXPECT_TRUE(a.IsSubset(b));
}

TEST_F(BitmapsetTest, IsSubsetFalse) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(4);
    b.AddMember(1).AddMember(2).AddMember(3);
    EXPECT_FALSE(a.IsSubset(b));
}

TEST_F(BitmapsetTest, BmsIsSubsetNullptrA) {
    Bitmapset b;
    b.AddMember(1);
    EXPECT_TRUE(bms_is_subset(nullptr, &b));  // empty ⊆ anything
}

TEST_F(BitmapsetTest, BmsIsSubsetNullptrB) {
    Bitmapset a;
    a.AddMember(1);
    EXPECT_FALSE(bms_is_subset(&a, nullptr));
    Bitmapset empty;
    EXPECT_TRUE(bms_is_subset(&empty, nullptr));  // empty ⊆ empty
}

TEST_F(BitmapsetTest, NonemptyDifferenceTrue) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2);
    b.AddMember(2).AddMember(3);
    EXPECT_TRUE(a.NonemptyDifference(b));  // a has 1 not in b
}

TEST_F(BitmapsetTest, NonemptyDifferenceFalse) {
    Bitmapset a, b;
    a.AddMember(2).AddMember(3);
    b.AddMember(1).AddMember(2).AddMember(3);
    EXPECT_FALSE(a.NonemptyDifference(b));  // a ⊆ b
}

TEST_F(BitmapsetTest, BmsNonemptyDifferenceNullptrA) {
    Bitmapset b;
    b.AddMember(1);
    EXPECT_FALSE(bms_nonempty_difference(nullptr, &b));
}

// --- Union / Intersect / Difference ----------------------------------------

TEST_F(BitmapsetTest, UnionCombines) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2);
    b.AddMember(2).AddMember(3);
    std::unique_ptr<Bitmapset> u(a.Union(b));
    EXPECT_EQ(u->NumMembers(), 3);
    EXPECT_TRUE(u->IsMember(1));
    EXPECT_TRUE(u->IsMember(2));
    EXPECT_TRUE(u->IsMember(3));
}

TEST_F(BitmapsetTest, BmsUnionWithNullptr) {
    Bitmapset a;
    a.AddMember(1).AddMember(2);
    std::unique_ptr<Bitmapset> u1(bms_union(&a, nullptr));
    ASSERT_NE(u1, nullptr);
    EXPECT_TRUE(u1->Equals(a));
    std::unique_ptr<Bitmapset> u2(bms_union(nullptr, &a));
    ASSERT_NE(u2, nullptr);
    EXPECT_TRUE(u2->Equals(a));
    EXPECT_EQ(bms_union(nullptr, nullptr), nullptr);
}

TEST_F(BitmapsetTest, IntersectCommon) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2).AddMember(3);
    b.AddMember(2).AddMember(3).AddMember(4);
    std::unique_ptr<Bitmapset> i(a.Intersect(b));
    EXPECT_EQ(i->NumMembers(), 2);
    EXPECT_TRUE(i->IsMember(2));
    EXPECT_TRUE(i->IsMember(3));
    EXPECT_FALSE(i->IsMember(1));
    EXPECT_FALSE(i->IsMember(4));
}

TEST_F(BitmapsetTest, IntersectDisjointIsEmpty) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2);
    b.AddMember(3).AddMember(4);
    std::unique_ptr<Bitmapset> i(a.Intersect(b));
    EXPECT_TRUE(i->IsEmpty());
}

TEST_F(BitmapsetTest, BmsIntersectWithNullptr) {
    Bitmapset a;
    a.AddMember(1);
    EXPECT_EQ(bms_intersect(&a, nullptr), nullptr);
    EXPECT_EQ(bms_intersect(nullptr, &a), nullptr);
}

TEST_F(BitmapsetTest, DifferenceRemoves) {
    Bitmapset a, b;
    a.AddMember(1).AddMember(2).AddMember(3);
    b.AddMember(2);
    std::unique_ptr<Bitmapset> d(a.Difference(b));
    EXPECT_EQ(d->NumMembers(), 2);
    EXPECT_TRUE(d->IsMember(1));
    EXPECT_TRUE(d->IsMember(3));
    EXPECT_FALSE(d->IsMember(2));
}

TEST_F(BitmapsetTest, BmsDifferenceWithNullptrB) {
    Bitmapset a;
    a.AddMember(1).AddMember(2);
    std::unique_ptr<Bitmapset> d(bms_difference(&a, nullptr));
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->Equals(a));
    EXPECT_EQ(bms_difference(nullptr, &a), nullptr);
}

// --- Copy / Free -----------------------------------------------------------

TEST_F(BitmapsetTest, BmsCopyIsDeep) {
    std::unique_ptr<Bitmapset> orig(bms_make_singleton(5));
    orig->AddMember(10);
    std::unique_ptr<Bitmapset> dup(bms_copy(orig.get()));
    ASSERT_NE(dup, nullptr);
    EXPECT_NE(dup.get(), orig.get());
    EXPECT_TRUE(dup->Equals(*orig));
    // Mutating the copy does not affect the original.
    dup->AddMember(99);
    EXPECT_FALSE(orig->IsMember(99));
}

TEST_F(BitmapsetTest, BmsCopyNullptr) {
    EXPECT_EQ(bms_copy(nullptr), nullptr);
}

TEST_F(BitmapsetTest, BmsFreeNullptrIsSafe) {
    bms_free(nullptr);  // must not crash
}

// --- NextMember iteration --------------------------------------------------

TEST_F(BitmapsetTest, NextMemberIteratesInOrder) {
    Bitmapset bms;
    bms.AddMember(5).AddMember(1).AddMember(100).AddMember(50);
    std::vector<int> members;
    int prev = -1;
    while (true) {
        int m = bms.NextMember(prev);
        if (m == -2) {
            break;
        }
        members.push_back(m);
        prev = m;
    }
    EXPECT_EQ(members, (std::vector<int>{1, 5, 50, 100}));
}

TEST_F(BitmapsetTest, NextMemberOnEmptyReturnsMinusTwo) {
    Bitmapset bms;
    EXPECT_EQ(bms.NextMember(-1), -2);
}

TEST_F(BitmapsetTest, BmsNextMemberNullptr) {
    EXPECT_EQ(bms_next_member(nullptr, -1), -2);
}

TEST_F(BitmapsetTest, NextMemberStartsAfterPrevbit) {
    Bitmapset bms;
    bms.AddMember(1).AddMember(2).AddMember(3);
    // prevbit=1 means "start looking from 2".
    EXPECT_EQ(bms.NextMember(1), 2);
    EXPECT_EQ(bms.NextMember(3), -2);
}

// --- Negative / boundary inputs --------------------------------------------

TEST_F(BitmapsetTest, IsMemberNegativeReturnsFalse) {
    Bitmapset bms;
    bms.AddMember(1);
    EXPECT_FALSE(bms.IsMember(-1));
    EXPECT_FALSE(bms_is_member(-1, &bms));
}

TEST_F(BitmapsetTest, AddMemberNegativeIsNoop) {
    Bitmapset bms;
    bms.AddMember(-5);
    EXPECT_TRUE(bms.IsEmpty());
}

TEST_F(BitmapsetTest, DelMemberNegativeIsNoop) {
    Bitmapset bms;
    bms.AddMember(1);
    bms.DelMember(-5);
    EXPECT_EQ(bms.NumMembers(), 1);
    EXPECT_TRUE(bms.IsMember(1));
}

// --- ToString (debug) ------------------------------------------------------

TEST_F(BitmapsetTest, ToStringEmpty) {
    Bitmapset bms;
    EXPECT_EQ(bms.ToString(), "{}");
}

TEST_F(BitmapsetTest, ToStringMultipleMembers) {
    Bitmapset bms;
    bms.AddMember(3).AddMember(1).AddMember(2);
    EXPECT_EQ(bms.ToString(), "{1,2,3}");
}

// --- Cross-word boundary (64-bit) ------------------------------------------

TEST_F(BitmapsetTest, CrossWordBoundary) {
    Bitmapset bms;
    bms.AddMember(63);
    bms.AddMember(64);   // first bit of the second word
    bms.AddMember(128);  // first bit of the third word
    EXPECT_EQ(bms.NumMembers(), 3);
    EXPECT_TRUE(bms.IsMember(63));
    EXPECT_TRUE(bms.IsMember(64));
    EXPECT_TRUE(bms.IsMember(128));
    EXPECT_EQ(bms.MinMember(), 63);
    EXPECT_EQ(bms.MaxMember(), 128);
    bms.DelMember(64);
    EXPECT_EQ(bms.NumMembers(), 2);
    EXPECT_FALSE(bms.IsMember(64));
}

}  // namespace
