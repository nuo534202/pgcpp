// syslogger.cpp — System logger: collects log messages in-memory.
//
// Converted from PostgreSQL 15's src/backend/postmaster/syslogger.c.
//
// The syslogger reads log lines from a pipe connected to all backend
// processes' stderr and writes them to a log file (or syslog). In MyToyDB
// (single-process), the syslogger is a stateful API: SysLoggerWrite
// queues a message; SysLoggerMain processes the queue.
#include "mytoydb/server/syslogger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "mytoydb/server/interrupt.hpp"

namespace mytoydb::server {

namespace {

SysLoggerStats& Stats() {
    static SysLoggerStats s;
    return s;
}

std::vector<LogMessage>& MessageQueue() {
    static std::vector<LogMessage> q;
    return q;
}

std::vector<LogMessage>& ProcessedMessages() {
    static std::vector<LogMessage> m;
    return m;
}

SysLoggerState& State() {
    static SysLoggerState s = SysLoggerState::kStopped;
    return s;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// NormalizeLevel — uppercase the level name for matching.
std::string NormalizeLevel(const std::string& level) {
    std::string out;
    out.reserve(level.size());
    for (char c : level) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// LevelIndex — 0=debug, 1=info, 2=notice, 3=warning, 4=error, 5=fatal.
// Returns -1 for unknown levels.
int LevelIndex(const std::string& level) {
    std::string n = NormalizeLevel(level);
    if (n == "DEBUG")
        return 0;
    if (n == "INFO")
        return 1;
    if (n == "NOTICE")
        return 2;
    if (n == "WARNING")
        return 3;
    if (n == "ERROR")
        return 4;
    if (n == "FATAL")
        return 5;
    return -1;
}

}  // namespace

void InitializeSysLogger() {
    Stats() = SysLoggerStats{};
    MessageQueue().clear();
    ProcessedMessages().clear();
    State() = SysLoggerState::kStopped;
}

void ResetSysLogger() {
    InitializeSysLogger();
}

void SysLoggerStart() {
    State() = SysLoggerState::kRunning;
    Stats().running = true;
}

void SysLoggerStop() {
    State() = SysLoggerState::kStopped;
    Stats().running = false;
}

void SysLoggerWrite(const std::string& level, const std::string& message) {
    LogMessage msg;
    msg.timestamp_ms = NowMs();
    msg.level = NormalizeLevel(level);
    msg.message = message;
    MessageQueue().push_back(std::move(msg));
}

int SysLoggerMain(int max_iterations) {
    if (State() != SysLoggerState::kRunning) {
        return 0;
    }

    int processed = 0;
    auto& queue = MessageQueue();
    auto& stats = Stats();

    for (int i = 0; i < max_iterations && !queue.empty(); ++i) {
        if (InterruptFlags::ShutdownRequested.load()) {
            break;
        }

        LogMessage msg = std::move(queue.front());
        queue.erase(queue.begin());

        // "Write" the message: in MyToyDB we just store it.
        ProcessedMessages().push_back(msg);

        ++stats.messages_logged;
        stats.bytes_written += msg.message.size();

        int idx = LevelIndex(msg.level);
        switch (idx) {
            case 0:
                ++stats.debug_count;
                break;
            case 1:
                ++stats.info_count;
                break;
            case 2:
                ++stats.notice_count;
                break;
            case 3:
                ++stats.warning_count;
                break;
            case 4:
                ++stats.error_count;
                break;
            case 5:
                ++stats.fatal_count;
                break;
            default:
                break;
        }
        ++processed;
    }

    return processed;
}

std::vector<LogMessage> GetSysLoggerMessages() {
    return ProcessedMessages();
}

SysLoggerState GetSysLoggerState() {
    return State();
}

SysLoggerStats GetSysLoggerStats() {
    return Stats();
}

}  // namespace mytoydb::server
