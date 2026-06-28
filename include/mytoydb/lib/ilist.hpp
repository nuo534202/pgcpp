// ilist.hpp — Intrusive linked lists.
//
// Mirrors PostgreSQL's src/backend/lib/ilist.c with a macro-free C++ port of
// the dlist (doubly-linked, circular head sentinel) and slist (singly-linked,
// null-terminated) data structures. The node structs are embedded inside
// the payload types they link, so no separate allocation is required for
// the linkage itself (the "intrusive" property).
//
// API parity with PostgreSQL:
//   dlist / DListHead:
//     dlist_push_head / PushHead        dlist_push_tail / PushTail
//     dlist_pop_head / PopHead         dlist_delete_node / Delete
//     dlist_empty / IsEmpty            dlist_is_first / IsFirst
//     dlist_is_last / IsLast           dlist_has_next / HasNext
//     dlist_container_head / Head      dlist_container_tail / Tail
//     dlist_foreach / Foreach
//   slist / SListHead:
//     slist_push_head / PushHead       slist_pop_head / PopHead
//     slist_is_empty / IsEmpty         slist_has_next / HasNext
//     slist_container_head / Head
//     slist_foreach / Foreach
//
// Differences from PostgreSQL:
//   - Macros (dlist_foreach, dlist_foreach_modify, slist_foreach) are
//     replaced by iterator classes usable in range-based for loops.
//   - The list head is a plain struct (no MemoryContext integration): nodes
//     are owned by the caller, and the head simply links existing nodes.

#pragma once

#include <cstddef>
#include <iterator>

namespace mytoydb::lib {

// ---------------------------------------------------------------------------
// Doubly-linked circular list (PostgreSQL: dlist_head / dlist_node).
// ---------------------------------------------------------------------------

struct DListNode;

struct DListNode {
    DListNode* prev;
    DListNode* next;

    // Initialize as a sentinel (self-linked).
    void Init() {
        prev = this;
        next = this;
    }
};

struct DListHead {
    DListNode head;  // sentinel; head.next is first real node, head.prev is last

    DListHead() { head.Init(); }
};

// Free functions (PG-style lowercase names).
void dlist_init(DListHead* head);
bool dlist_is_empty(const DListHead* head);
DListNode* dlist_head_node(DListHead* head);
DListNode* dlist_tail_node(DListHead* head);
DListNode* dlist_next_node(DListHead* head, DListNode* node);
DListNode* dlist_prev_node(DListHead* head, DListNode* node);
bool dlist_is_first(DListHead* head, DListNode* node);
bool dlist_is_last(DListHead* head, DListNode* node);
bool dlist_has_next(DListHead* head, DListNode* node);
bool dlist_has_prev(DListHead* head, DListNode* node);
void dlist_push_head(DListHead* head, DListNode* node);
void dlist_push_tail(DListHead* head, DListNode* node);
DListNode* dlist_pop_head(DListHead* head);
DListNode* dlist_pop_tail(DListHead* head);
void dlist_delete_node(DListHead* head, DListNode* node);
void dlist_move_tail(DListHead* src, DListHead* dst);
void dlist_clear(DListHead* head);
int dlist_length(const DListHead* head);

// Cast helper (PG: dlist_container).
template<typename T, DListNode T::*Member>
inline T* dlist_container(DListNode* node) {
    if (node == nullptr) {
        return nullptr;
    }
    // offsetof a member of non-standard-layout type is technically UB, but
    // we use the conventional byte-arithmetic form that all major compilers
    // accept and that PostgreSQL itself relies on.
    auto offset = reinterpret_cast<std::size_t>(&(static_cast<T*>(nullptr)->*Member));
    return reinterpret_cast<T*>(reinterpret_cast<char*>(node) - offset);
}

// C++ convenience class wrapping a DListHead with iterator support.
class DList {
public:
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = DListNode*;
        using difference_type = std::ptrdiff_t;
        using pointer = DListNode**;
        using reference = DListNode*;

        Iterator() = default;
        explicit Iterator(DListNode* node) : node_(node) {}
        reference operator*() const { return node_; }
        Iterator& operator++() {
            node_ = node_->next;
            return *this;
        }
        Iterator operator++(int) {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }
        bool operator==(const Iterator& other) const { return node_ == other.node_; }
        bool operator!=(const Iterator& other) const { return node_ != other.node_; }

    private:
        DListNode* node_ = nullptr;
    };

    DList() = default;

    void PushHead(DListNode* node) { dlist_push_head(&head_, node); }
    void PushTail(DListNode* node) { dlist_push_tail(&head_, node); }
    DListNode* PopHead() { return dlist_pop_head(&head_); }
    DListNode* PopTail() { return dlist_pop_tail(&head_); }
    void Delete(DListNode* node) { dlist_delete_node(&head_, node); }
    bool IsEmpty() const { return dlist_is_empty(&head_); }
    int Length() const { return dlist_length(&head_); }
    void Clear() { dlist_clear(&head_); }

    DListNode* Head() { return dlist_head_node(&head_); }
    DListNode* Tail() { return dlist_tail_node(&head_); }

    Iterator begin() { return Iterator(head_.head.next); }
    Iterator end() { return Iterator(&head_.head); }

private:
    DListHead head_;
};

// ---------------------------------------------------------------------------
// Singly-linked list (PostgreSQL: slist_head / slist_node).
// ---------------------------------------------------------------------------

struct SListNode {
    SListNode* next;
};

struct SListHead {
    SListNode head;  // sentinel; head.next is first real node, head.next==&head means end

    SListHead() { head.next = &head; }
};

void slist_init(SListHead* head);
bool slist_is_empty(const SListHead* head);
SListNode* slist_head_node(SListHead* head);
SListNode* slist_pop_head(SListHead* head);
void slist_push_head(SListHead* head, SListNode* node);
bool slist_has_next(SListHead* head, SListNode* node);
SListNode* slist_next_node(SListHead* head, SListNode* node);
int slist_length(const SListHead* head);

class SList {
public:
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = SListNode*;
        using difference_type = std::ptrdiff_t;
        using pointer = SListNode**;
        using reference = SListNode*;

        Iterator() = default;
        explicit Iterator(SListNode* node) : node_(node) {}
        reference operator*() const { return node_; }
        Iterator& operator++() {
            node_ = node_->next;
            return *this;
        }
        Iterator operator++(int) {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }
        bool operator==(const Iterator& other) const { return node_ == other.node_; }
        bool operator!=(const Iterator& other) const { return node_ != other.node_; }

    private:
        SListNode* node_ = nullptr;
    };

    SList() = default;

    void PushHead(SListNode* node) { slist_push_head(&head_, node); }
    SListNode* PopHead() { return slist_pop_head(&head_); }
    bool IsEmpty() const { return slist_is_empty(&head_); }
    int Length() const { return slist_length(&head_); }

    Iterator begin() { return Iterator(head_.head.next); }
    Iterator end() { return Iterator(&head_.head); }

private:
    SListHead head_;
};

}  // namespace mytoydb::lib
