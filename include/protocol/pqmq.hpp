// pqmq.h — libpq shared-message-queue interconnect (pqmq.c).
//
// Converted from PostgreSQL 15's src/backend/libpq/pqmq.c.
//
// PG uses shared-memory message queues (shm_mq.c) to ship frontend protocol
// messages between the leader backend and parallel query workers. pqmq.c is
// the adapter: it implements the same `pq_*` API (pq_putmessage, pq_copybytes,
// etc.) but routes bytes to a shm_mq instead of a socket.
//
// pgcpp runs single-process; parallel workers are simulated by in-process
// queues. pqmq.c is therefore a thin adapter that buffers outgoing bytes
// in a per-queue std::string and lets another end pop them. The API mirrors
// PG's so the executor's parallel-aware nodes can call pq_putmessage etc.
#pragma once

#include <cstdint>
#include <string>

#include "protocol/pqformat.hpp"

namespace pgcpp::protocol {

// PqMqQueue — a one-way byte channel between a "leader" (writer) and a
// "worker" (reader). Bytes written by the leader are buffered until the
// worker reads them.
//
// In PG this is a shm_mq; in pgcpp it is a std::string protected by a
// process-wide mutex (since pgcpp is single-process, the mutex is for
// tests that may use multiple function calls).
struct PqMqQueue {
    std::string buf;
    bool closed = false;
    // Total bytes written / read (for statistics).
    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
};

// pq_mq_attach — attach a queue to the current backend's "pq" output sink.
// After this, pq_putmessage will route to the queue instead of the socket.
void pq_mq_attach(PqMqQueue* queue);

// pq_mq_detach — detach the current queue (further pq_putmessage calls go
// to the socket sink).
void pq_mq_detach();

// pq_mq_putmessage — append a wire-format message (type byte + length + payload)
// to the attached queue. Returns 0 on success, EOF on error.
int pq_mq_putmessage(char msgtype, const char* s, size_t len);

// pq_mq_read_bytes — read up to `len` bytes from the queue into `buf`.
// Returns the number of bytes read (0 if the queue is empty and not closed,
// -1 if closed and drained).
int pq_mq_read_bytes(PqMqQueue* queue, char* buf, size_t len);

// pq_mq_close — mark the queue as closed (no more writes will succeed).
void pq_mq_close(PqMqQueue* queue);

// pq_mq_get_attached — return the currently-attached queue (or nullptr).
PqMqQueue* pq_mq_get_attached();

// ResetPqMqState — detach any queue and clear global state (for tests).
void ResetPqMqState();

}  // namespace pgcpp::protocol
