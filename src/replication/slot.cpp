// slot.cpp — Replication slots.
//
// Converted from PostgreSQL 15's src/backend/replication/slot.c.
//
// PG stores slots in shared memory; pgcpp keeps an in-process
// std::map<std::string, ReplicationSlot> keyed by slot name.
#include "replication/slot.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/error/elog.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::replication {

using pgcpp::error::LogLevel;

namespace {

ReplicationSlotCtlData& Ctl() {
    static ReplicationSlotCtlData c;
    return c;
}

// Directory for persistent slot files.
// Empty means "no persistence" (tests that don't set a directory).
std::string& Dir() {
    static std::string dir;
    return dir;
}

// Build the directory path for a slot: <base_dir>/<slot_name>/
std::string SlotDir(const std::string& name) {
    if (Dir().empty()) {
        return "";
    }
    return Dir() + "/" + name;
}

// Build the state file path for a slot: <base_dir>/<slot_name>/state
std::string SlotStateFile(const std::string& name) {
    if (Dir().empty()) {
        return "";
    }
    return SlotDir(name) + "/state";
}

// Serialize a ReplicationSlot to its on-disk state file.
// Format (text, one key=value per line):
//   name=<string>
//   type=<physical|logical>
//   persistence=<persistent|temporary>
//   plugin=<string>
//   database=<string>
//   xmin=<uint>
//   catalog_xmin=<uint>
//   restart_lsn=<uint>
//   confirmed_flush_lsn=<uint>
void WriteSlotFile(const ReplicationSlot& s) {
    if (Dir().empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(SlotDir(s.name), ec);

    std::string path = SlotStateFile(s.name);
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << "name=" << s.name << "\n";
    out << "type=" << SlotTypeName(s.type) << "\n";
    out << "persistence=" << SlotPersistenceName(s.persistence) << "\n";
    out << "plugin=" << s.plugin << "\n";
    out << "database=" << s.database << "\n";
    out << "xmin=" << s.xmin << "\n";
    out << "catalog_xmin=" << s.catalog_xmin << "\n";
    out << "restart_lsn=" << s.restart_lsn << "\n";
    out << "confirmed_flush_lsn=" << s.confirmed_flush_lsn << "\n";
    out.flush();
}

// Remove a slot's on-disk state directory.
void RemoveSlotDir(const std::string& name) {
    if (Dir().empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(SlotDir(name), ec);
}

// Parse a single "key=value" line and apply it to a ReplicationSlot.
void ParseSlotLine(const std::string& line, ReplicationSlot& s) {
    auto eq = line.find('=');
    if (eq == std::string::npos) {
        return;
    }
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    if (key == "name") {
        s.name = val;
    } else if (key == "type") {
        s.type = (val == "logical") ? SlotType::kLogical : SlotType::kPhysical;
    } else if (key == "persistence") {
        s.persistence =
            (val == "persistent") ? SlotPersistence::kPersistent : SlotPersistence::kTemporary;
    } else if (key == "plugin") {
        s.plugin = val;
    } else if (key == "database") {
        s.database = val;
    } else if (key == "xmin") {
        s.xmin = static_cast<transaction::TransactionId>(std::strtoull(val.c_str(), nullptr, 10));
    } else if (key == "catalog_xmin") {
        s.catalog_xmin =
            static_cast<transaction::TransactionId>(std::strtoull(val.c_str(), nullptr, 10));
    } else if (key == "restart_lsn") {
        s.restart_lsn =
            static_cast<transaction::XLogRecPtr>(std::strtoull(val.c_str(), nullptr, 10));
    } else if (key == "confirmed_flush_lsn") {
        s.confirmed_flush_lsn =
            static_cast<transaction::XLogRecPtr>(std::strtoull(val.c_str(), nullptr, 10));
    }
}

// Load a single slot from its state file.
bool ReadSlotFile(const std::string& path, ReplicationSlot& s) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        ParseSlotLine(line, s);
    }
    return true;
}

}  // namespace

void ReplicationSlotInit() {
    Ctl().slots.clear();
}

void ReplicationSlotReset() {
    ReplicationSlotInit();
}

bool ReplicationSlotCreate(std::string name, SlotType type, SlotPersistence persistence,
                           std::string plugin, std::string database) {
    if (name.empty()) {
        ereport(LogLevel::kError, "cannot create replication slot: name is empty");
        return false;
    }
    if (Ctl().slots.count(name) != 0) {
        ereport(LogLevel::kError,
                "cannot create replication slot: slot \"" + name + "\" already exists");
        return false;
    }
    if (type == SlotType::kLogical && plugin.empty()) {
        ereport(LogLevel::kError, "cannot create logical replication slot: plugin is required");
        return false;
    }
    ReplicationSlot s;
    s.name = std::move(name);
    s.type = type;
    s.persistence = persistence;
    s.plugin = std::move(plugin);
    s.database = std::move(database);
    s.restart_lsn = transaction::GetXLogInsertRecPtr();
    s.confirmed_flush_lsn = s.restart_lsn;
    s.dirty = true;
    Ctl().slots[s.name] = s;
    WriteSlotFile(s);
    return true;
}

bool ReplicationSlotAcquire(const std::string& name, int32_t pid) {
    auto it = Ctl().slots.find(name);
    if (it == Ctl().slots.end()) {
        ereport(LogLevel::kError,
                "cannot acquire replication slot: slot \"" + name + "\" does not exist");
        return false;
    }
    if (it->second.active) {
        ereport(LogLevel::kError,
                "cannot acquire replication slot: slot \"" + name + "\" is active");
        return false;
    }
    it->second.active = true;
    it->second.active_pid = pid;
    it->second.just_started = true;
    it->second.dirty = true;
    return true;
}

std::string ReplicationSlotRelease(int32_t pid) {
    for (auto& [name, slot] : Ctl().slots) {
        if (slot.active && slot.active_pid == pid) {
            slot.active = false;
            slot.active_pid = 0;
            slot.just_started = false;
            slot.dirty = true;
            return name;
        }
    }
    return {};
}

bool ReplicationSlotDrop(const std::string& name) {
    auto it = Ctl().slots.find(name);
    if (it == Ctl().slots.end()) {
        ereport(LogLevel::kError,
                "cannot drop replication slot: slot \"" + name + "\" does not exist");
        return false;
    }
    if (it->second.active) {
        ereport(LogLevel::kError, "cannot drop replication slot: slot \"" + name + "\" is active");
        return false;
    }
    Ctl().slots.erase(it);
    RemoveSlotDir(name);
    return true;
}

bool ReplicationSlotPersist(const std::string& name) {
    auto it = Ctl().slots.find(name);
    if (it == Ctl().slots.end()) {
        ereport(LogLevel::kError,
                "cannot persist replication slot: slot \"" + name + "\" does not exist");
        return false;
    }
    it->second.persistence = SlotPersistence::kPersistent;
    it->second.dirty = true;
    return true;
}

transaction::XLogRecPtr ReplicationSlotAdvance(const std::string& name,
                                               transaction::XLogRecPtr upto) {
    auto it = Ctl().slots.find(name);
    if (it == Ctl().slots.end()) {
        ereport(LogLevel::kError,
                "cannot advance replication slot: slot \"" + name + "\" does not exist");
        return 0;
    }
    ReplicationSlot& s = it->second;
    transaction::XLogRecPtr new_lsn = 0;
    if (s.type == SlotType::kPhysical) {
        if (upto > s.restart_lsn) {
            s.restart_lsn = upto;
        }
        new_lsn = s.restart_lsn;
    } else {
        if (upto > s.confirmed_flush_lsn) {
            s.confirmed_flush_lsn = upto;
        }
        new_lsn = s.confirmed_flush_lsn;
    }
    s.dirty = true;
    WriteSlotFile(s);
    return new_lsn;
}

const ReplicationSlot* ReplicationSlotLookup(const std::string& name) {
    auto it = Ctl().slots.find(name);
    if (it == Ctl().slots.end()) {
        return nullptr;
    }
    return &it->second;
}

int ReplicationSlotCount() {
    return static_cast<int>(Ctl().slots.size());
}

ReplicationSlotCtlData* GetReplicationSlotCtl() {
    return &Ctl();
}

const char* SlotTypeName(SlotType t) {
    return (t == SlotType::kLogical) ? "logical" : "physical";
}

const char* SlotPersistenceName(SlotPersistence p) {
    return (p == SlotPersistence::kPersistent) ? "persistent" : "temporary";
}

// --- Persistence (pg_replslot/) ---

void SetReplicationSlotDirectory(const std::string& dir) {
    Dir() = dir;
}

void ReplicationSlotLoadSlots() {
    if (Dir().empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(Dir(), ec)) {
        return;  // fresh initdb — no slots
    }
    for (const auto& entry : std::filesystem::directory_iterator(Dir(), ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        std::string state_file = entry.path().string() + "/state";
        ReplicationSlot s;
        if (ReadSlotFile(state_file, s) && !s.name.empty()) {
            s.dirty = false;
            Ctl().slots[s.name] = std::move(s);
        }
    }
}

void ReplicationSlotFlushSlots() {
    if (Dir().empty()) {
        return;
    }
    for (auto& [name, s] : Ctl().slots) {
        if (s.dirty) {
            WriteSlotFile(s);
            s.dirty = false;
        }
    }
}

void ReplicationSlotResetPersistence() {
    Ctl().slots.clear();
    if (!Dir().empty()) {
        std::error_code ec;
        std::filesystem::remove_all(Dir(), ec);
    }
}

}  // namespace pgcpp::replication
