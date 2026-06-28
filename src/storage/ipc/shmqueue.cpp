// shmqueue.cpp — Doubly-linked list in shared memory.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/shmqueue.c.
#include "pgcpp/storage/ipc/shmqueue.hpp"

namespace mytoydb::storage {

void SHMQueueInit(SHMQueue* queue) {
    queue->head.prev = &queue->head;
    queue->head.next = &queue->head;
}

bool SHMQueueEmpty(const SHMQueue* queue) {
    return queue->head.next == &queue->head;
}

void SHMQueuePush(SHMQueue* queue, SHMQueueElem* elem) {
    // Insert at the head (PG's SHMQueueInsertAtHead).
    elem->prev = &queue->head;
    elem->next = queue->head.next;
    queue->head.next->prev = elem;
    queue->head.next = elem;
}

void SHMQueuePushTail(SHMQueue* queue, SHMQueueElem* elem) {
    elem->next = &queue->head;
    elem->prev = queue->head.prev;
    queue->head.prev->next = elem;
    queue->head.prev = elem;
}

SHMQueueElem* SHMQueuePop(SHMQueue* queue) {
    if (SHMQueueEmpty(queue)) {
        return nullptr;
    }
    SHMQueueElem* elem = queue->head.next;
    SHMQueueDelete(elem);
    return elem;
}

void SHMQueueDelete(SHMQueueElem* elem) {
    if (elem->prev != nullptr) {
        elem->prev->next = elem->next;
    }
    if (elem->next != nullptr) {
        elem->next->prev = elem->prev;
    }
    elem->prev = nullptr;
    elem->next = nullptr;
}

int SHMQueueLength(const SHMQueue* queue) {
    int count = 0;
    for (SHMQueueElem* p = queue->head.next; p != nullptr && p != &queue->head; p = p->next) {
        ++count;
    }
    return count;
}

}  // namespace mytoydb::storage
