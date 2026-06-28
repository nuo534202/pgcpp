// ilist.cpp — Intrusive doubly/singly linked list implementations.
//
// Mirrors PostgreSQL's src/backend/lib/ilist.c. The list heads are circular
// sentinel nodes: for DList, head.next points to the first real node and
// head.prev to the last, with an empty list being head.next == head.prev ==
// &head. For SList we use a self-pointing sentinel so end() is the head
// pointer itself.

#include "pgcpp/lib/ilist.hpp"

namespace pgcpp::lib {

// ---------------------------------------------------------------------------
// Doubly-linked list (dlist).
// ---------------------------------------------------------------------------

void dlist_init(DListHead* head) {
    head->head.Init();
}

bool dlist_is_empty(const DListHead* head) {
    return head->head.next == &head->head;
}

DListNode* dlist_head_node(DListHead* head) {
    if (dlist_is_empty(head)) {
        return nullptr;
    }
    return head->head.next;
}

DListNode* dlist_tail_node(DListHead* head) {
    if (dlist_is_empty(head)) {
        return nullptr;
    }
    return head->head.prev;
}

DListNode* dlist_next_node(DListHead* head, DListNode* node) {
    (void)head;
    return node->next;
}

DListNode* dlist_prev_node(DListHead* head, DListNode* node) {
    (void)head;
    return node->prev;
}

bool dlist_is_first(DListHead* head, DListNode* node) {
    return head->head.next == node;
}

bool dlist_is_last(DListHead* head, DListNode* node) {
    return head->head.prev == node;
}

bool dlist_has_next(DListHead* head, DListNode* node) {
    return node->next != &head->head;
}

bool dlist_has_prev(DListHead* head, DListNode* node) {
    return node->prev != &head->head;
}

void dlist_push_head(DListHead* head, DListNode* node) {
    node->next = head->head.next;
    node->prev = &head->head;
    head->head.next->prev = node;
    head->head.next = node;
}

void dlist_push_tail(DListHead* head, DListNode* node) {
    node->prev = head->head.prev;
    node->next = &head->head;
    head->head.prev->next = node;
    head->head.prev = node;
}

DListNode* dlist_pop_head(DListHead* head) {
    if (dlist_is_empty(head)) {
        return nullptr;
    }
    DListNode* node = head->head.next;
    dlist_delete_node(head, node);
    return node;
}

DListNode* dlist_pop_tail(DListHead* head) {
    if (dlist_is_empty(head)) {
        return nullptr;
    }
    DListNode* node = head->head.prev;
    dlist_delete_node(head, node);
    return node;
}

void dlist_delete_node(DListHead* head, DListNode* node) {
    (void)head;
    node->prev->next = node->next;
    node->next->prev = node->prev;
    // Leave the node in a safe self-linked state.
    node->next = node;
    node->prev = node;
}

void dlist_move_tail(DListHead* src, DListHead* dst) {
    if (dlist_is_empty(src)) {
        dlist_init(dst);
        return;
    }
    dst->head.next = src->head.next;
    dst->head.prev = src->head.prev;
    src->head.next->prev = &dst->head;
    src->head.prev->next = &dst->head;
    dlist_init(src);
}

void dlist_clear(DListHead* head) {
    dlist_init(head);
}

int dlist_length(const DListHead* head) {
    int count = 0;
    for (DListNode* node = head->head.next; node != &head->head; node = node->next) {
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Singly-linked list (slist).
// ---------------------------------------------------------------------------

void slist_init(SListHead* head) {
    head->head.next = &head->head;
}

bool slist_is_empty(const SListHead* head) {
    return head->head.next == &head->head;
}

SListNode* slist_head_node(SListHead* head) {
    if (slist_is_empty(head)) {
        return nullptr;
    }
    return head->head.next;
}

void slist_push_head(SListHead* head, SListNode* node) {
    node->next = head->head.next;
    head->head.next = node;
}

SListNode* slist_pop_head(SListHead* head) {
    if (slist_is_empty(head)) {
        return nullptr;
    }
    SListNode* node = head->head.next;
    head->head.next = node->next;
    node->next = nullptr;
    return node;
}

bool slist_has_next(SListHead* head, SListNode* node) {
    return node->next != &head->head;
}

SListNode* slist_next_node(SListHead* head, SListNode* node) {
    (void)head;
    return node->next;
}

int slist_length(const SListHead* head) {
    int count = 0;
    for (SListNode* node = head->head.next; node != &head->head; node = node->next) {
        ++count;
    }
    return count;
}

}  // namespace pgcpp::lib
