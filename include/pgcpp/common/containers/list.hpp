#pragma once

#include <cstddef>
#include <vector>

namespace pgcpp::containers {

// Forward declaration for Node (used in typed List<T>)
namespace nodes {
class Node;
}

// List — a dynamic list of void* pointers, backed by std::vector.
// Faithful conversion of PostgreSQL's List. Keeps the same API names.
// Elements are stored as void* (PostgreSQL stores Datum which is uintptr_t).
class List {
public:
    List() = default;

    // Append to end (PostgreSQL lappend)
    void Append(void* datum);
    // Prepend to start (PostgreSQL lcons)
    void Prepend(void* datum);

    // Get element at index (PostgreSQL list_nth)
    void* Get(std::size_t index) const;
    // Get the first element (PostgreSQL linitial)
    void* First() const;
    // Get the last element (PostgreSQL llast)
    void* Last() const;

    // Number of elements (PostgreSQL list_length)
    std::size_t Length() const { return data_.size(); }
    bool IsEmpty() const { return data_.empty(); }

    // Delete element at index (PostgreSQL list_delete_nth_cell)
    void Delete(std::size_t index);
    // Delete all elements matching datum (PostgreSQL list_delete_ptr)
    void DeletePtr(void* datum);

    // Concatenate: returns a new list (PostgreSQL list_concat)
    // Modifies this list in place (appends other's elements).
    void Concat(const List& other);

    // Check membership (PostgreSQL list_member_ptr)
    bool Member(void* datum) const;

    // Reverse in place (PostgreSQL list_reverse)
    void Reverse();

    // Iterator access for range-based for loops
    void** Begin() { return data_.data(); }
    void** End() { return data_.data() + data_.size(); }
    void* const* Begin() const { return data_.data(); }
    void* const* End() const { return data_.data() + data_.size(); }

    // Direct element access (for foreach-style iteration)
    void* operator[](std::size_t index) const { return data_[index]; }
    void*& operator[](std::size_t index) { return data_[index]; }

private:
    std::vector<void*> data_;
};

// PostgreSQL-compatible API functions (lowercase, kept for compatibility).
// These operate on List* (heap-allocated via palloc).
List* lappend(List* list, void* datum);
List* lcons(List* list, void* datum);
void* list_nth(const List* list, int index);
void* linitial(const List* list);
void* llast(const List* list);
int list_length(const List* list);
List* list_delete_nth_cell(List* list, int index);
List* list_concat(List* list1, const List* list2);
bool list_member_ptr(const List* list, void* datum);
List* list_reverse(List* list);

// Create a new empty list (allocated via palloc).
List* newList();

// foreach macro — PostgreSQL-compatible iteration.
// Usage:
//   ListCell* cell;
//   foreach(cell, list) { void* datum = lfirst(cell); ... }
// In C++, we simplify: the "cell" is an index variable.
#define foreach(cell, list) for (std::size_t cell = 0; (cell) < (list)->Length(); ++(cell))

#define lfirst(cell) ((list)->Get(cell))

// Typed List<T> — C++ convenience template for type-safe lists.
template<typename T>
class TypedList {
public:
    void Append(T* item) { list_.Append(static_cast<void*>(item)); }
    T* Get(std::size_t index) const { return static_cast<T*>(list_.Get(index)); }
    std::size_t Length() const { return list_.Length(); }
    bool IsEmpty() const { return list_.IsEmpty(); }

    // Allow implicit conversion to List* for PostgreSQL API compatibility
    operator List*() { return &list_; }

private:
    List list_;
};

}  // namespace pgcpp::containers
