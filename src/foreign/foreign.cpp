// foreign.cpp — FDW catalog and handler registry implementation.
//
// Converted from PostgreSQL 15's src/backend/foreign/foreign.c.
//
// Maintains in-memory stores for foreign servers, user mappings, and
// foreign tables, plus the FDW handler registry that maps FDW names
// (e.g., "file_fdw") to FdwRoutine factory functions.
#include "foreign/foreign.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/error/elog.hpp"
#include "foreign/fdwapi.hpp"

namespace pgcpp::foreign {

namespace {

// --- FDW catalog storage (function-local statics for lazy init) ---

std::vector<ForeignServer>& Servers() {
    static std::vector<ForeignServer> v;
    return v;
}

std::vector<UserMapping>& Mappings() {
    static std::vector<UserMapping> v;
    return v;
}

std::vector<ForeignTable>& Tables() {
    static std::vector<ForeignTable> v;
    return v;
}

// OID counter for foreign servers and user mappings. Foreign tables reuse
// the pg_class OID, so they don't need a separate counter.
pgcpp::catalog::Oid& NextOid() {
    static pgcpp::catalog::Oid oid = pgcpp::catalog::kFirstNormalObjectId;
    return oid;
}

pgcpp::catalog::Oid AllocateOid() {
    return NextOid()++;
}

// --- FDW handler registry ---

std::unordered_map<std::string, FdwRoutineFactory>& Registry() {
    static std::unordered_map<std::string, FdwRoutineFactory> r;
    return r;
}

}  // namespace

// --- Foreign server API ---

pgcpp::catalog::Oid CreateForeignServer(const std::string& servername, const std::string& fdwname,
                                        pgcpp::catalog::Oid owner,
                                        const std::vector<FdwOption>& options) {
    for (const auto& s : Servers()) {
        if (s.servername == servername) {
            char errbuf[256];
            std::snprintf(errbuf, sizeof(errbuf), "foreign server \"%s\" already exists",
                          servername.c_str());
            ereport(pgcpp::error::LogLevel::kError, errbuf);
        }
    }
    ForeignServer s;
    s.serverid = AllocateOid();
    s.servername = servername;
    s.fdwname = fdwname;
    s.owner = owner;
    s.options = options;
    Servers().push_back(std::move(s));
    return Servers().back().serverid;
}

const ForeignServer* LookupForeignServerByName(const std::string& servername) {
    for (const auto& s : Servers()) {
        if (s.servername == servername) {
            return &s;
        }
    }
    return nullptr;
}

const ForeignServer* LookupForeignServerByOid(pgcpp::catalog::Oid serverid) {
    for (const auto& s : Servers()) {
        if (s.serverid == serverid) {
            return &s;
        }
    }
    return nullptr;
}

bool DropForeignServer(pgcpp::catalog::Oid serverid) {
    auto& servers = Servers();
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        if (it->serverid == serverid) {
            servers.erase(it);
            // Cascade: remove user mappings and foreign tables referencing
            // this server.
            auto& mappings = Mappings();
            mappings.erase(
                std::remove_if(mappings.begin(), mappings.end(),
                               [serverid](const UserMapping& m) { return m.serverid == serverid; }),
                mappings.end());
            auto& tables = Tables();
            tables.erase(std::remove_if(
                             tables.begin(), tables.end(),
                             [serverid](const ForeignTable& t) { return t.serverid == serverid; }),
                         tables.end());
            return true;
        }
    }
    return false;
}

std::size_t NumForeignServers() {
    return Servers().size();
}

// --- User mapping API ---

pgcpp::catalog::Oid CreateUserMapping(pgcpp::catalog::Oid serverid, pgcpp::catalog::Oid userid,
                                      const std::vector<FdwOption>& options) {
    if (LookupForeignServerByOid(serverid) == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "server OID %u does not exist", serverid);
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    if (LookupUserMapping(serverid, userid) != nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "user mapping already exists for server %u, user %u",
                      serverid, userid);
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    UserMapping m;
    m.umid = AllocateOid();
    m.serverid = serverid;
    m.userid = userid;
    m.options = options;
    Mappings().push_back(std::move(m));
    return Mappings().back().umid;
}

const UserMapping* LookupUserMapping(pgcpp::catalog::Oid serverid, pgcpp::catalog::Oid userid) {
    for (const auto& m : Mappings()) {
        if (m.serverid == serverid && m.userid == userid) {
            return &m;
        }
    }
    return nullptr;
}

bool DropUserMapping(pgcpp::catalog::Oid umid) {
    auto& mappings = Mappings();
    for (auto it = mappings.begin(); it != mappings.end(); ++it) {
        if (it->umid == umid) {
            mappings.erase(it);
            return true;
        }
    }
    return false;
}

std::size_t NumUserMappings() {
    return Mappings().size();
}

// --- Foreign table API ---

pgcpp::catalog::Oid CreateForeignTable(pgcpp::catalog::Oid relid, pgcpp::catalog::Oid serverid,
                                       const std::vector<FdwOption>& options) {
    if (LookupForeignServerByOid(serverid) == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "server OID %u does not exist", serverid);
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    if (LookupForeignTable(relid) != nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "foreign table with relid %u already exists", relid);
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    ForeignTable t;
    t.relid = relid;
    t.serverid = serverid;
    t.options = options;
    Tables().push_back(std::move(t));
    return relid;
}

const ForeignTable* LookupForeignTable(pgcpp::catalog::Oid relid) {
    for (const auto& t : Tables()) {
        if (t.relid == relid) {
            return &t;
        }
    }
    return nullptr;
}

bool DropForeignTable(pgcpp::catalog::Oid relid) {
    auto& tables = Tables();
    for (auto it = tables.begin(); it != tables.end(); ++it) {
        if (it->relid == relid) {
            tables.erase(it);
            return true;
        }
    }
    return false;
}

std::size_t NumForeignTables() {
    return Tables().size();
}

// --- Reset ---

void ResetForeignCatalog() {
    Servers().clear();
    Mappings().clear();
    Tables().clear();
    NextOid() = pgcpp::catalog::kFirstNormalObjectId;
}

// --- Option helpers ---

const std::string* GetOption(const std::vector<FdwOption>& options, const std::string& name) {
    for (const auto& opt : options) {
        if (opt.optname == name) {
            return &opt.optvalue;
        }
    }
    return nullptr;
}

// --- FDW handler registry ---

void RegisterFdw(const std::string& fdwname, FdwRoutineFactory factory) {
    if (Registry().count(fdwname) > 0) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "FDW \"%s\" is already registered", fdwname.c_str());
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    Registry()[fdwname] = factory;
}

const FdwRoutine* LookupFdw(const std::string& fdwname) {
    auto it = Registry().find(fdwname);
    if (it == Registry().end()) {
        return nullptr;
    }
    return it->second();
}

void ClearFdwRegistry() {
    Registry().clear();
}

std::size_t NumRegisteredFdws() {
    return Registry().size();
}

}  // namespace pgcpp::foreign
