// pgarch.cpp — WAL archiver: archives completed WAL segment files.
//
// Converted from PostgreSQL 15's src/backend/postmaster/pgarch.c.
//
// The archiver runs in a loop, polling for completed WAL segment files
// and archiving each via the configured archive_command (see
// shell_archive.h). In MyToyDB (single-process), the archiver maintains
// a queue of pending archive requests; PgArchiverMain processes them.
#include "pgcpp/server/pgarch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pgcpp/server/interrupt.hpp"
#include "pgcpp/server/shell_archive.hpp"

namespace mytoydb::server {

namespace {

std::vector<std::string>& ArchiveQueue() {
    static std::vector<std::string> q;
    return q;
}

PgArchStats& Stats() {
    static PgArchStats s;
    return s;
}

PgArchState& State() {
    static PgArchState s = PgArchState::kStopped;
    return s;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void InitializePgArch() {
    ArchiveQueue().clear();
    Stats() = PgArchStats{};
    State() = PgArchState::kStopped;
}

void ResetPgArch() {
    InitializePgArch();
}

void PgArchStart() {
    State() = PgArchState::kRunning;
    Stats().running = true;
}

void PgArchStop() {
    State() = PgArchState::kStopped;
    Stats().running = false;
}

int PgArchiverMain(int max_iterations) {
    if (State() == PgArchState::kStopped) {
        return 0;
    }

    State() = PgArchState::kRunning;
    int archived = 0;

    for (int i = 0; i < max_iterations && !ArchiveQueue().empty(); ++i) {
        if (InterruptFlags::ShutdownRequested.load()) {
            break;
        }

        std::string file = std::move(ArchiveQueue().front());
        ArchiveQueue().erase(ArchiveQueue().begin());

        State() = PgArchState::kArchiving;
        bool ok = PgArchiveWALFile(file);
        State() = PgArchState::kRunning;

        if (ok) {
            ++archived;
        } else {
            // Re-queue the failed file for retry.
            ArchiveQueue().push_back(file);
            break;  // Stop on first failure.
        }
    }

    State() = PgArchState::kRunning;
    return archived;
}

bool PgArchiveWALFile(const std::string& file_path) {
    // Call the shell archiver with the configured archive_command.
    int rc = ShellArchive(file_path, /*last=*/"");
    auto& s = Stats();
    if (rc == 0) {
        ++s.files_archived;
        s.last_archive_time_ms = NowMs();
        s.last_archived_file = file_path;
        return true;
    }
    ++s.archive_failures;
    return false;
}

bool QueueArchiveRequest(const std::string& file_path) {
    auto& q = ArchiveQueue();
    if (std::find(q.begin(), q.end(), file_path) != q.end()) {
        return false;
    }
    q.push_back(file_path);
    return true;
}

std::vector<std::string> GetPendingArchiveRequests() {
    return ArchiveQueue();
}

PgArchState GetPgArchState() {
    return State();
}

PgArchStats GetPgArchStats() {
    return Stats();
}

}  // namespace mytoydb::server
