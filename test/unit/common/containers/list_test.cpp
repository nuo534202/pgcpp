#include "common/containers/list.hpp"

#include <gtest/gtest.h>

#include <string>

#include "common/containers/node.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace {

using pgcpp::containers::lappend;
using pgcpp::containers::lcons;
using pgcpp::containers::linitial;
using pgcpp::containers::List;
using pgcpp::containers::list_concat;
using pgcpp::containers::list_delete_nth_cell;
using pgcpp::containers::list_length;
using pgcpp::containers::list_member_ptr;
using pgcpp::containers::list_nth;
using pgcpp::containers::list_reverse;
using pgcpp::containers::llast;
using pgcpp::containers::newList;
using pgcpp::containers::TypedList;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::ContextSwitchGuard;

// Helper to properly destroy a palloc'd List (unregister destructor, then
// call destructor and pfree).
void DestroyList(List* list) {
    if (list != nullptr) {
        pgcpp::nodes::destroyPallocNode(list);
    }
}

class ListTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = AllocSetContext::Create("list_test_context");
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

// --- List method tests (stack-allocated) ---

TEST_F(ListTest, AppendAndLength) {
    List list;
    int a = 1, b = 2, c = 3;
    list.Append(&a);
    list.Append(&b);
    list.Append(&c);
    EXPECT_EQ(list.Length(), 3u);
    EXPECT_FALSE(list.IsEmpty());
}

TEST_F(ListTest, GetByIndex) {
    List list;
    int a = 10, b = 20, c = 30;
    list.Append(&a);
    list.Append(&b);
    list.Append(&c);
    EXPECT_EQ(list.Get(0), &a);
    EXPECT_EQ(list.Get(1), &b);
    EXPECT_EQ(list.Get(2), &c);
}

TEST_F(ListTest, GetOutOfRangeReturnsNull) {
    List list;
    int a = 1;
    list.Append(&a);
    EXPECT_EQ(list.Get(5), nullptr);
    EXPECT_EQ(list.Get(0), &a);
}

TEST_F(ListTest, Prepend) {
    List list;
    int a = 1, b = 2, c = 3;
    list.Prepend(&a);
    list.Prepend(&b);
    list.Prepend(&c);
    EXPECT_EQ(list.Length(), 3u);
    EXPECT_EQ(list.Get(0), &c);
    EXPECT_EQ(list.Get(1), &b);
    EXPECT_EQ(list.Get(2), &a);
}

TEST_F(ListTest, FirstAndLast) {
    List list;
    int a = 1, b = 2, c = 3;
    list.Append(&a);
    list.Append(&b);
    list.Append(&c);
    EXPECT_EQ(list.First(), &a);
    EXPECT_EQ(list.Last(), &c);
}

TEST_F(ListTest, FirstAndLastOnEmpty) {
    List list;
    EXPECT_EQ(list.First(), nullptr);
    EXPECT_EQ(list.Last(), nullptr);
}

TEST_F(ListTest, DeleteByIndex) {
    List list;
    int a = 1, b = 2, c = 3;
    list.Append(&a);
    list.Append(&b);
    list.Append(&c);
    list.Delete(1);
    EXPECT_EQ(list.Length(), 2u);
    EXPECT_EQ(list.Get(0), &a);
    EXPECT_EQ(list.Get(1), &c);
}

TEST_F(ListTest, DeleteOutOfRangeIsNoop) {
    List list;
    int a = 1;
    list.Append(&a);
    list.Delete(5);
    EXPECT_EQ(list.Length(), 1u);
}

TEST_F(ListTest, DeletePtr) {
    List list;
    int a = 1, b = 2, c = 3, d = 4;
    list.Append(&a);
    list.Append(&b);
    list.Append(&c);
    list.Append(&d);
    list.DeletePtr(&b);
    EXPECT_EQ(list.Length(), 3u);
    EXPECT_EQ(list.Get(0), &a);
    EXPECT_EQ(list.Get(1), &c);
    EXPECT_EQ(list.Get(2), &d);
}

TEST_F(ListTest, DeletePtrRemovesAllMatching) {
    List list;
    int a = 1, b = 2, c = 3;
    list.Append(&a);
    list.Append(&b);
    list.Append(&c);
    list.Append(&b);  // same pointer appended twice
    list.DeletePtr(&b);
    EXPECT_EQ(list.Length(), 2u);
    EXPECT_EQ(list.Get(0), &a);
    EXPECT_EQ(list.Get(1), &c);
}

TEST_F(ListTest, Concat) {
    List list1, list2;
    int a = 1, b = 2, c = 3, d = 4;
    list1.Append(&a);
    list1.Append(&b);
    list2.Append(&c);
    list2.Append(&d);
    list1.Concat(list2);
    EXPECT_EQ(list1.Length(), 4u);
    EXPECT_EQ(list1.Get(0), &a);
    EXPECT_EQ(list1.Get(1), &b);
    EXPECT_EQ(list1.Get(2), &c);
    EXPECT_EQ(list1.Get(3), &d);
    // list2 is unchanged.
    EXPECT_EQ(list2.Length(), 2u);
}

TEST_F(ListTest, Member) {
    List list;
    int a = 1, b = 2, c = 3;
    list.Append(&a);
    list.Append(&b);
    list.Append(&c);
    EXPECT_TRUE(list.Member(&a));
    EXPECT_TRUE(list.Member(&b));
    EXPECT_TRUE(list.Member(&c));
    int x = 99;
    EXPECT_FALSE(list.Member(&x));
}

TEST_F(ListTest, MemberOnEmpty) {
    List list;
    int a = 1;
    EXPECT_FALSE(list.Member(&a));
}

TEST_F(ListTest, Reverse) {
    List list;
    int a = 1, b = 2, c = 3;
    list.Append(&a);
    list.Append(&b);
    list.Append(&c);
    list.Reverse();
    EXPECT_EQ(list.Get(0), &c);
    EXPECT_EQ(list.Get(1), &b);
    EXPECT_EQ(list.Get(2), &a);
}

TEST_F(ListTest, SubscriptOperator) {
    List list;
    int a = 1, b = 2;
    list.Append(&a);
    list.Append(&b);
    EXPECT_EQ(list[0], &a);
    EXPECT_EQ(list[1], &b);
    int c = 3;
    list[1] = &c;
    EXPECT_EQ(list[1], &c);
}

// --- PostgreSQL-compatible lowercase API tests ---

TEST_F(ListTest, LappendCreatesNewList) {
    int a = 42;
    List* list = lappend(nullptr, &a);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list_length(list), 1);
    EXPECT_EQ(list_nth(list, 0), &a);
    DestroyList(list);
}

TEST_F(ListTest, LappendOnExistingList) {
    int a = 1, b = 2, c = 3;
    List* list = nullptr;
    list = lappend(list, &a);
    list = lappend(list, &b);
    list = lappend(list, &c);
    EXPECT_EQ(list_length(list), 3);
    EXPECT_EQ(list_nth(list, 0), &a);
    EXPECT_EQ(list_nth(list, 1), &b);
    EXPECT_EQ(list_nth(list, 2), &c);
    DestroyList(list);
}

TEST_F(ListTest, LconsCreatesNewList) {
    int a = 42;
    List* list = lcons(nullptr, &a);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list_length(list), 1);
    EXPECT_EQ(list_nth(list, 0), &a);
    DestroyList(list);
}

TEST_F(ListTest, LconsPrepends) {
    int a = 1, b = 2, c = 3;
    List* list = nullptr;
    list = lcons(list, &a);
    list = lcons(list, &b);
    list = lcons(list, &c);
    EXPECT_EQ(list_length(list), 3);
    EXPECT_EQ(list_nth(list, 0), &c);
    EXPECT_EQ(list_nth(list, 1), &b);
    EXPECT_EQ(list_nth(list, 2), &a);
    DestroyList(list);
}

TEST_F(ListTest, LinitialAndLlast) {
    int a = 1, b = 2, c = 3;
    List* list = nullptr;
    list = lappend(list, &a);
    list = lappend(list, &b);
    list = lappend(list, &c);
    EXPECT_EQ(linitial(list), &a);
    EXPECT_EQ(llast(list), &c);
    DestroyList(list);
}

TEST_F(ListTest, LinitialAndLlastOnNull) {
    EXPECT_EQ(linitial(nullptr), nullptr);
    EXPECT_EQ(llast(nullptr), nullptr);
}

TEST_F(ListTest, ListLengthOnNull) {
    EXPECT_EQ(list_length(nullptr), 0);
}

TEST_F(ListTest, ListNthOnNull) {
    EXPECT_EQ(list_nth(nullptr, 0), nullptr);
}

TEST_F(ListTest, ListDeleteNthCell) {
    int a = 1, b = 2, c = 3;
    List* list = nullptr;
    list = lappend(list, &a);
    list = lappend(list, &b);
    list = lappend(list, &c);
    list = list_delete_nth_cell(list, 1);
    EXPECT_EQ(list_length(list), 2);
    EXPECT_EQ(list_nth(list, 0), &a);
    EXPECT_EQ(list_nth(list, 1), &c);
    DestroyList(list);
}

TEST_F(ListTest, ListConcat) {
    int a = 1, b = 2, c = 3, d = 4;
    List* list1 = nullptr;
    list1 = lappend(list1, &a);
    list1 = lappend(list1, &b);
    List* list2 = nullptr;
    list2 = lappend(list2, &c);
    list2 = lappend(list2, &d);
    list1 = list_concat(list1, list2);
    EXPECT_EQ(list_length(list1), 4);
    EXPECT_EQ(list_nth(list1, 0), &a);
    EXPECT_EQ(list_nth(list1, 3), &d);
    // Only destroy list1; list2's elements were copied (pointers only).
    DestroyList(list1);
    DestroyList(list2);
}

TEST_F(ListTest, ListConcatWithNullFirst) {
    int a = 1, b = 2;
    List* list2 = nullptr;
    list2 = lappend(list2, &a);
    list2 = lappend(list2, &b);
    List* result = list_concat(nullptr, list2);
    EXPECT_EQ(result, list2);
    DestroyList(list2);
}

TEST_F(ListTest, ListMemberPtr) {
    int a = 1, b = 2, c = 3;
    List* list = nullptr;
    list = lappend(list, &a);
    list = lappend(list, &b);
    EXPECT_TRUE(list_member_ptr(list, &a));
    EXPECT_TRUE(list_member_ptr(list, &b));
    EXPECT_FALSE(list_member_ptr(list, &c));
    EXPECT_FALSE(list_member_ptr(nullptr, &a));
    DestroyList(list);
}

TEST_F(ListTest, ListReverse) {
    int a = 1, b = 2, c = 3;
    List* list = nullptr;
    list = lappend(list, &a);
    list = lappend(list, &b);
    list = lappend(list, &c);
    list = list_reverse(list);
    EXPECT_EQ(list_nth(list, 0), &c);
    EXPECT_EQ(list_nth(list, 1), &b);
    EXPECT_EQ(list_nth(list, 2), &a);
    DestroyList(list);
}

TEST_F(ListTest, NewList) {
    List* list = newList();
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list_length(list), 0);
    EXPECT_TRUE(list->IsEmpty());
    DestroyList(list);
}

// --- foreach macro test ---
// NOTE: lfirst(cell) expands to ((list)->Get(cell)), so the list variable
// must be named "list" for the macro to work.

TEST_F(ListTest, ForeachMacro) {
    int a = 1, b = 2, c = 3;
    List* list = nullptr;
    list = lappend(list, &a);
    list = lappend(list, &b);
    list = lappend(list, &c);

    int count = 0;
    foreach (cell, list) {
        void* datum = lfirst(cell);
        EXPECT_NE(datum, nullptr);
        ++count;
    }
    EXPECT_EQ(count, 3);
    DestroyList(list);
}

TEST_F(ListTest, ForeachMacroCollectsValues) {
    int a = 10, b = 20, c = 30;
    List* list = nullptr;
    list = lappend(list, &a);
    list = lappend(list, &b);
    list = lappend(list, &c);

    int sum = 0;
    foreach (cell, list) {
        auto* val = static_cast<int*>(lfirst(cell));
        sum += *val;
    }
    EXPECT_EQ(sum, 60);
    DestroyList(list);
}

// --- TypedList<T> template test ---

TEST_F(ListTest, TypedListBasic) {
    struct Item {
        int id;
    };
    Item i1{1};
    Item i2{2};
    Item i3{3};

    TypedList<Item> typed;
    typed.Append(&i1);
    typed.Append(&i2);
    typed.Append(&i3);

    EXPECT_EQ(typed.Length(), 3u);
    EXPECT_FALSE(typed.IsEmpty());

    EXPECT_EQ(typed.Get(0)->id, 1);
    EXPECT_EQ(typed.Get(1)->id, 2);
    EXPECT_EQ(typed.Get(2)->id, 3);
}

TEST_F(ListTest, TypedListEmpty) {
    struct Item {
        int id;
    };
    TypedList<Item> typed;
    EXPECT_EQ(typed.Length(), 0u);
    EXPECT_TRUE(typed.IsEmpty());
}

TEST_F(ListTest, TypedListConvertsToListPtr) {
    struct Item {
        int id;
    };
    Item i1{42};
    TypedList<Item> typed;
    typed.Append(&i1);

    // Implicit conversion to List*
    List* list = typed;
    EXPECT_EQ(list_length(list), 1);
    EXPECT_EQ(list_nth(list, 0), &i1);
}

}  // namespace
