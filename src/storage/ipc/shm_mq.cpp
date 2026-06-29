// shm_mq.cpp — Shared-memory message queue (ring buffer).
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/shm_mq.c.
#include "storage/ipc/shm_mq.hpp"

#include <algorithm>
#include <cstring>

namespace pgcpp::storage {

ShmMq::ShmMq(std::size_t size_bytes) : buffer_(size_bytes, 0) {}

void ShmMq::Reset() {
    if (!buffer_.empty()) {
        std::memset(buffer_.data(), 0, buffer_.size());
    }
    read_pos_ = 0;
    write_pos_ = 0;
    bytes_queued_ = 0;
}

ShmMqResult ShmMq::Send(const uint8_t* data, std::size_t nbytes, bool non_blocking) {
    if (buffer_.empty()) {
        return ShmMqResult::kDetached;
    }
    if (bytes_queued_ + nbytes > buffer_.size()) {
        if (non_blocking) {
            return ShmMqResult::kWouldBlock;
        }
        // In pgcpp we never block; truncate to what fits.
        nbytes = buffer_.size() - bytes_queued_;
    }
    // Copy in chunks to handle wrap-around.
    std::size_t first_chunk = std::min(nbytes, buffer_.size() - write_pos_);
    std::memcpy(buffer_.data() + write_pos_, data, first_chunk);
    if (first_chunk < nbytes) {
        std::memcpy(buffer_.data(), data + first_chunk, nbytes - first_chunk);
        write_pos_ = nbytes - first_chunk;
    } else {
        write_pos_ += first_chunk;
        if (write_pos_ == buffer_.size()) {
            write_pos_ = 0;
        }
    }
    bytes_queued_ += nbytes;
    return ShmMqResult::kSuccess;
}

ShmMqResult ShmMq::Receive(uint8_t* data, std::size_t nbytes, std::size_t* received,
                           bool non_blocking) {
    if (bytes_queued_ == 0) {
        if (received != nullptr) {
            *received = 0;
        }
        return non_blocking ? ShmMqResult::kWouldBlock : ShmMqResult::kSuccess;
    }
    std::size_t to_read = std::min(nbytes, bytes_queued_);
    std::size_t first_chunk = std::min(to_read, buffer_.size() - read_pos_);
    std::memcpy(data, buffer_.data() + read_pos_, first_chunk);
    if (first_chunk < to_read) {
        std::memcpy(data + first_chunk, buffer_.data(), to_read - first_chunk);
        read_pos_ = to_read - first_chunk;
    } else {
        read_pos_ += first_chunk;
        if (read_pos_ == buffer_.size()) {
            read_pos_ = 0;
        }
    }
    bytes_queued_ -= to_read;
    if (received != nullptr) {
        *received = to_read;
    }
    return ShmMqResult::kSuccess;
}

ShmMq* ShmMqCreate(std::size_t size_bytes) {
    return new ShmMq(size_bytes);
}

void ShmMqDestroy(ShmMq* mq) {
    delete mq;
}

}  // namespace pgcpp::storage
