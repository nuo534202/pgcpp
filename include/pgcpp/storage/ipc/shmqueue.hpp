// shmqueue.h — Doubly-linked list in shared memory.
//
// Converted from PostgreSQL 15's src/include/storage/shmqueue.h and
// src/backend/storage/ipc/shmqueue.c.
//
// A SHMQueue is a doubly-linked list whose nodes (SHMQueueElem) live in
// shared memory. The queue links are pointers (SHMQueueElem*) — in PG they
// would be SHM offsets, but pgcpp uses raw pointers since the regions live
// in the same address space.
//
// Used by PGPROC (proc list),PredicateLockTarget (predicate locks), and
// various other subsystems.
#pragma once

namespace pgcpp::storage {

// SHMQueueElem — one node in a doubly-linked list.
// Embed this in your struct (PG uses intrusive lists).
struct SHMQueueElem {
    SHMQueueElem* prev = nullptr;
    SHMQueueElem* next = nullptr;

    SHMQueueElem() = default;
};

// SHMQueue — the list head. PG treats the head as a SHMQueueElem too; we
// preserve that pattern (head.prev/next are the first/last elements).
struct SHMQueue {
    SHMQueueElem head;  // sentinel: head.next = first, head.prev = last

    SHMQueue() {  // empty list points back at itself
        head.prev = &head;
        head.next = &head;
    }
};

// SHMQueueInit — initialize a queue to empty.
void SHMQueueInit(SHMQueue* queue);

// SHMQueueEmpty — true if the queue contains no elements.
bool SHMQueueEmpty(const SHMQueue* queue);

// SHMQueuePush — push elem at the head of the queue (PG's SHMQueueInsertAtHead).
void SHMQueuePush(SHMQueue* queue, SHMQueueElem* elem);

// SHMQueuePushTail — push elem at the tail of the queue.
void SHMQueuePushTail(SHMQueue* queue, SHMQueueElem* elem);

// SHMQueuePop — pop and return the head element, or nullptr if empty.
SHMQueueElem* SHMQueuePop(SHMQueue* queue);

// SHMQueueDelete — unlink elem from its queue.
void SHMQueueDelete(SHMQueueElem* elem);

// SHMQueueLength — count the number of elements (O(n)).
int SHMQueueLength(const SHMQueue* queue);

}  // namespace pgcpp::storage
