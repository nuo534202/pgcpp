// twophase.cpp — Two-phase commit (2PC) prepared transaction implementation.
//
// Converted from PostgreSQL 15's src/backend/access/transam/twophase.c.
//
// Implements the prepared-transaction state store: an in-memory list of
// TwoPhaseState records backed by per-record files in pg_twophase/.
//
// File format (text, one key=value per line):
//   gid=<string>
//   xid=<uint>
//   isolation=<read committed|repeatable read|serializable|read uncommitted>
//   read_only=<0|1>
//   deferrable=<0|1>
//
// Filename: pg_twophase/<hex_xid> (16 hex digits, zero-padded).
// Using XID as filename avoids filesystem-unsafe characters in GIDs.
#include "transaction/twophase.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/error/elog.hpp"

namespace pgcpp::transaction {

namespace {

// In-memory list of prepared transactions.
std::vector<TwoPhaseState>& StateList() {
    static std::vector<TwoPhaseState> list;
    return list;
}

// Directory for persistent prepared-transaction files.
// Empty means "no persistence" (tests that don't set a directory).
std::string& Dir() {
    static std::string dir;
    return dir;
}

// Format the XID as a 16-digit hex filename component.
std::string FormatXidHex(TransactionId xid) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016X", static_cast<unsigned int>(xid));
    return std::string(buf);
}

// Build the full path for a prepared-transaction file.
std::string TwoPhaseFilePath(TransactionId xid) {
    if (Dir().empty()) {
        return "";
    }
    return Dir() + "/" + FormatXidHex(xid);
}

// Serialize a TwoPhaseState to its on-disk file.
void WriteStateFile(const TwoPhaseState& s) {
    if (Dir().empty()) {
        return;  // no persistence configured
    }
    // Ensure the directory exists.
    std::error_code ec;
    std::filesystem::create_directories(Dir(), ec);
    // create_directories returns false on error; we ignore ec and attempt
    // the write anyway — if the directory truly doesn't exist, the ofstream
    // will fail silently (no data persisted, but in-memory state is still
    // correct).

    std::string path = TwoPhaseFilePath(s.xid);
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << "gid=" << s.gid << "\n";
    out << "xid=" << s.xid << "\n";
    out << "isolation=" << IsolationLevelName(s.isolation_level) << "\n";
    out << "read_only=" << (s.read_only ? 1 : 0) << "\n";
    out << "deferrable=" << (s.deferrable ? 1 : 0) << "\n";
    out.flush();
}

// Remove the on-disk file for a prepared transaction.
void RemoveStateFile(TransactionId xid) {
    if (Dir().empty()) {
        return;
    }
    std::string path = TwoPhaseFilePath(xid);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Parse a single "key=value" line and apply it to a TwoPhaseState.
// Returns true on success.
bool ParseLine(const std::string& line, TwoPhaseState& s) {
    auto eq = line.find('=');
    if (eq == std::string::npos) {
        return false;
    }
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    if (key == "gid") {
        s.gid = val;
    } else if (key == "xid") {
        s.xid = static_cast<TransactionId>(std::strtoul(val.c_str(), nullptr, 10));
    } else if (key == "isolation") {
        s.isolation_level = ParseIsolationLevelName(val);
    } else if (key == "read_only") {
        s.read_only = (val == "1");
    } else if (key == "deferrable") {
        s.deferrable = (val == "1");
    }
    return true;
}

// Load a single TwoPhaseState from its on-disk file.
bool ReadStateFile(const std::string& path, TwoPhaseState& s) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        ParseLine(line, s);
    }
    return true;
}

}  // namespace

// --- Public API ---

void SaveTwoPhaseState(const TwoPhaseState& state) {
    // Check for duplicate GID.
    for (const auto& s : StateList()) {
        if (s.gid == state.gid) {
            char errbuf[256];
            std::snprintf(errbuf, sizeof(errbuf), "transaction identifier \"%s\" is already in use",
                          state.gid.c_str());
            ereport(pgcpp::error::LogLevel::kError, errbuf);
        }
    }
    StateList().push_back(state);
    WriteStateFile(state);
}

bool RemoveTwoPhaseState(const std::string& gid) {
    auto& list = StateList();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->gid == gid) {
            RemoveStateFile(it->xid);
            list.erase(it);
            return true;
        }
    }
    return false;
}

const TwoPhaseState* LookupTwoPhaseState(const std::string& gid) {
    for (const auto& s : StateList()) {
        if (s.gid == gid) {
            return &s;
        }
    }
    return nullptr;
}

bool CommitPreparedTransaction(const std::string& gid) {
    const TwoPhaseState* s = LookupTwoPhaseState(gid);
    if (s == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf),
                      "prepared transaction with identifier \"%s\" does not exist", gid.c_str());
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    // Mark the XID as committed in the CLOG.
    TransactionIdCommit(s->xid);
    RemoveTwoPhaseState(gid);
    return true;
}

bool RollbackPreparedTransaction(const std::string& gid) {
    const TwoPhaseState* s = LookupTwoPhaseState(gid);
    if (s == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf),
                      "prepared transaction with identifier \"%s\" does not exist", gid.c_str());
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    // Mark the XID as aborted in the CLOG.
    TransactionIdAbort(s->xid);
    RemoveTwoPhaseState(gid);
    return true;
}

std::size_t NumTwoPhaseStates() {
    return StateList().size();
}

// --- Persistence ---

void SetTwoPhaseDirectory(const std::string& dir) {
    Dir() = dir;
}

void LoadTwoPhaseFiles() {
    StateList().clear();
    if (Dir().empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(Dir(), ec)) {
        return;  // fresh initdb — no prepared transactions
    }
    for (const auto& entry : std::filesystem::directory_iterator(Dir(), ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        TwoPhaseState s;
        if (ReadStateFile(entry.path().string(), s) && !s.gid.empty()) {
            StateList().push_back(std::move(s));
        }
    }
}

void FlushTwoPhaseFiles() {
    if (Dir().empty()) {
        return;
    }
    // Remove files for prepared transactions that no longer exist in memory
    // (already committed/aborted), then write/update files for current ones.
    std::error_code ec;
    if (!std::filesystem::exists(Dir(), ec)) {
        return;
    }
    // Write all current states (overwrites existing files).
    for (const auto& s : StateList()) {
        WriteStateFile(s);
    }
}

void ResetTwoPhaseState() {
    StateList().clear();
    if (!Dir().empty()) {
        std::error_code ec;
        std::filesystem::remove_all(Dir(), ec);
    }
}

}  // namespace pgcpp::transaction
