// pqmq.cpp — Shared-memory message queue adapter for parallel workers.
//
// pgcpp runs single-process, so pqmq is a thin adapter that buffers bytes
// in a per-queue std::string. The leader backend calls pq_mq_attach() to
// route pq_putmessage calls into the queue; a worker reads them via
// pq_mq_read_bytes().
#include "protocol/pqmq.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace pgcpp::protocol {

namespace {

std::mutex& GlobalMutex() {
    static std::mutex m;
    return m;
}

PqMqQueue*& AttachedQueue() {
    static PqMqQueue* q = nullptr;
    return q;
}

}  // namespace

void pq_mq_attach(PqMqQueue* queue) {
    std::lock_guard<std::mutex> g(GlobalMutex());
    AttachedQueue() = queue;
}

void pq_mq_detach() {
    std::lock_guard<std::mutex> g(GlobalMutex());
    AttachedQueue() = nullptr;
}

int pq_mq_putmessage(char msgtype, const char* s, size_t len) {
    std::lock_guard<std::mutex> g(GlobalMutex());
    PqMqQueue* q = AttachedQueue();
    if (q == nullptr || q->closed)
        return -1;
    // Wire format: type byte + 4-byte length (big-endian) + payload.
    q->buf.push_back(msgtype);
    int32_t total = static_cast<int32_t>(4 + len);
    q->buf.push_back(static_cast<char>((total >> 24) & 0xff));
    q->buf.push_back(static_cast<char>((total >> 16) & 0xff));
    q->buf.push_back(static_cast<char>((total >> 8) & 0xff));
    q->buf.push_back(static_cast<char>(total & 0xff));
    q->buf.append(s, len);
    q->bytes_written += 1 + 4 + len;
    return 0;
}

int pq_mq_read_bytes(PqMqQueue* queue, char* buf, size_t len) {
    if (queue == nullptr)
        return -1;
    std::lock_guard<std::mutex> g(GlobalMutex());
    if (queue->buf.empty()) {
        return queue->closed ? -1 : 0;
    }
    size_t to_read = std::min(len, queue->buf.size());
    std::memcpy(buf, queue->buf.data(), to_read);
    queue->buf.erase(0, to_read);
    queue->bytes_read += to_read;
    return static_cast<int>(to_read);
}

void pq_mq_close(PqMqQueue* queue) {
    if (queue == nullptr)
        return;
    std::lock_guard<std::mutex> g(GlobalMutex());
    queue->closed = true;
}

PqMqQueue* pq_mq_get_attached() {
    std::lock_guard<std::mutex> g(GlobalMutex());
    return AttachedQueue();
}

void ResetPqMqState() {
    std::lock_guard<std::mutex> g(GlobalMutex());
    AttachedQueue() = nullptr;
}

}  // namespace pgcpp::protocol
