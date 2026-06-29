// shm_mq.h — Shared-memory message queue.
//
// Converted from PostgreSQL 15's src/include/storage/shm_mq.h and
// src/backend/storage/ipc/shm_mq.c.
//
// shm_mq is a single-producer/single-consumer ring buffer used by parallel
// query infrastructure to pass tuples between worker processes. The queue
// lives in shared memory and supports non-blocking send/receive.
//
// pgcpp is single-process, so the queue is a std::vector<uint8_t> ring
// buffer in heap memory. The API mirrors PG's shm_mq_send / shm_mq_receive.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pgcpp::storage {

// ShmMqResult — return code from Send/Receive (mirrors PG's shm_mq_result).
enum class ShmMqResult {
    kSuccess,     // SHM_MQ_SUCCESS — operation completed
    kWouldBlock,  // SHM_MQ_WOULD_BLOCK — would block in non-blocking mode
    kDetached,    // SHM_MQ_DETACHED — the other end has detached
};

// ShmMq — a single-producer/single-consumer byte queue (ring buffer).
class ShmMq {
public:
    ShmMq() = default;
    explicit ShmMq(std::size_t size_bytes);

    // Reset the queue to empty (does not deallocate).
    void Reset();

    // Size — total capacity in bytes.
    std::size_t Size() const { return buffer_.size(); }

    // Bytes — number of bytes currently in the queue (unread).
    std::size_t Bytes() const { return bytes_queued_; }

    // IsEmpty — true if no bytes are queued.
    bool IsEmpty() const { return bytes_queued_ == 0; }

    // Send — enqueue nbytes bytes; returns kSuccess or kWouldBlock.
    // If non_blocking and not enough space, returns kWouldBlock without
    // modifying the queue.
    ShmMqResult Send(const uint8_t* data, std::size_t nbytes, bool non_blocking);

    // Receive — dequeue up to nbytes bytes into *data; writes the actual
    // number of bytes received to *received. If non_blocking and the queue
    // is empty, returns kWouldBlock.
    ShmMqResult Receive(uint8_t* data, std::size_t nbytes, std::size_t* received,
                        bool non_blocking);

private:
    std::vector<uint8_t> buffer_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
    std::size_t bytes_queued_ = 0;
};

// ShmMqCreate — create a queue of the given size (PG's shm_mq_create equivalent).
ShmMq* ShmMqCreate(std::size_t size_bytes);

// ShmMqDestroy — destroy a queue created by ShmMqCreate.
void ShmMqDestroy(ShmMq* mq);

}  // namespace pgcpp::storage
