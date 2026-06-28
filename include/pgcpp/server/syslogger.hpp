// syslogger.h — System logger: captures stderr output from all backends.
//
// Converted from PostgreSQL 15's src/backend/postmaster/syslogger.c.
//
// PostgreSQL runs a syslogger process that reads log lines from a pipe
// connected to all backend processes' stderr. This centralizes logging so
// that log lines from concurrent backends are interleaved into a single
// file (or syslog) rather than scattered across the terminal.
//
// In MyToyDB (single-process), the syslogger is a stateful API that collects
// log messages into an in-memory buffer. SysLoggerWrite queues a message;
// SysLoggerMain processes the queue.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mytoydb::server {

// SysLoggerState — state of the system logger.
enum class SysLoggerState {
    // kStopped — logger not started.
    kStopped,
    // kRunning — logger in main loop, collecting messages.
    kRunning,
};

// LogDestination — bitmask of enabled log destinations.
// Matches PostgreSQL's LOG_DESTINATION_* constants.
enum LogDestination : uint32_t {
    kLogDestStderr = 1u << 0,
    kLogDestSyslog = 1u << 1,
    kLogDestEventlog = 1u << 2,
    kLogDestCsvlog = 1u << 3,
};

// LogMessage — a single logged message.
struct LogMessage {
    // Timestamp (ms since epoch) when the message was logged.
    int64_t timestamp_ms = 0;
    // Severity level ("DEBUG", "INFO", "NOTICE", "WARNING", "ERROR", "FATAL").
    std::string level;
    // The message body (no trailing newline).
    std::string message;
};

// SysLoggerStats — statistics tracked by the system logger.
struct SysLoggerStats {
    // Number of messages logged.
    uint64_t messages_logged = 0;
    // Number of bytes written (sum of message lengths).
    uint64_t bytes_written = 0;
    // Number of messages at each severity level.
    uint64_t debug_count = 0;
    uint64_t info_count = 0;
    uint64_t notice_count = 0;
    uint64_t warning_count = 0;
    uint64_t error_count = 0;
    uint64_t fatal_count = 0;
    // Whether the logger is currently running its main loop.
    bool running = false;
};

// InitializeSysLogger — set up logger state (clear buffer and stats).
void InitializeSysLogger();

// ResetSysLogger — clear logger state, buffer, and statistics (for testing).
void ResetSysLogger();

// SysLoggerStart — start the system logger (sets state to kRunning).
void SysLoggerStart();

// SysLoggerStop — stop the system logger (sets state to kStopped).
void SysLoggerStop();

// SysLoggerMain — main loop of the syslogger (simplified: processes
// pending messages up to `max_iterations` of them). Returns the number
// of messages processed.
int SysLoggerMain(int max_iterations);

// SysLoggerWrite — log a message to the system logger.
// `level` should be one of "DEBUG", "INFO", "NOTICE", "WARNING", "ERROR",
// "FATAL" (case-insensitive). The message is queued for processing.
void SysLoggerWrite(const std::string& level, const std::string& message);

// GetSysLoggerMessages — return a copy of all logged messages (for testing).
std::vector<LogMessage> GetSysLoggerMessages();

// GetSysLoggerState — return the current logger state.
SysLoggerState GetSysLoggerState();

// GetSysLoggerStats — return the current logger statistics.
SysLoggerStats GetSysLoggerStats();

}  // namespace mytoydb::server
