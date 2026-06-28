// extended_stats.cpp — implementation of the extended statistics API.
//
// Converts the public API of PostgreSQL's src/backend/statistics/extended_stats.c
// to C++20. The original C code drives the CREATE/ALTER/DROP STATISTICS DDL
// and dispatches the per-kind build/estimate calls through a stats provider
// registry. Here we keep the same surface (catalog cache, statement structs,
// provider registry) using std::map and std::vector.
//
// Simplifications (consistent with the rest of pgcpp):
//   - The catalog is an in-memory std::map rather than pg_statistic_ext rows.
//   - Name resolution is by the dotted qualified name stored at create time;
//     the full namespace/lookup machinery is not yet wired up.
//   - stxkeys are assigned 1-based attnums by column position in the statement
//     (the parser would normally resolve names to attnums via the relation's
//     attribute list).

#include "pgcpp/statistics/extended_stats.hpp"

#include <map>
#include <string>

#include "pgcpp/common/error/elog.hpp"

namespace pgcpp::statistics {

using pgcpp::error::LogLevel;

namespace {

// Provider registry. A function-local static avoids static-initialization-order
// issues and lets tests ClearRegistry() reliably.
std::map<StatsExtKind, StatisticsProvider*>& ProviderRegistry() {
    static std::map<StatsExtKind, StatisticsProvider*> registry;
    return registry;
}

// Join a qualified name ("schema.name" or just "name").
std::string JoinName(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += ".";
        out += parts[i];
    }
    return out;
}

}  // namespace

// === StatisticsCatalog ===

StatisticsCatalog& StatisticsCatalog::Instance() {
    static StatisticsCatalog instance;
    return instance;
}

void StatisticsCatalog::SetNextOid(Oid next) {
    next_oid_ = next;
}
Oid StatisticsCatalog::GetNextOid() const {
    return next_oid_;
}

Oid StatisticsCatalog::CreateStatistics(const CreateStatisticsStmt& stmt) {
    // PostgreSQL requires at least two columns for extended statistics.
    if (stmt.stxattrs.size() < 2) {
        ereport(LogLevel::kError, "cannot create extended statistics: at least 2 columns required");
    }

    std::string name = JoinName(stmt.stxnames);
    if (name.empty()) {
        ereport(LogLevel::kError, "cannot create extended statistics: no name given");
    }

    // Duplicate-name handling.
    for (const auto& [oid, data] : cache_) {
        if (data.stxname == name) {
            if (stmt.if_not_exists)
                return oid;
            ereport(LogLevel::kError, "statistics object \"" + name + "\" already exists");
        }
    }

    Oid oid = next_oid_++;
    StatsExtData data;
    data.stxoid = oid;
    data.stxname = name;
    data.stxnamespace = 0;
    data.stxrelid = stmt.stxrelid;
    data.stxstattarget = -1;
    data.stxkind = stmt.kinds;
    // Assign attnums 1..N by column position in the statement.
    data.stxkeys.reserve(stmt.stxattrs.size());
    for (size_t i = 0; i < stmt.stxattrs.size(); ++i) {
        data.stxkeys.push_back(static_cast<AttrNumber>(i + 1));
    }
    cache_[oid] = std::move(data);
    return oid;
}

void StatisticsCatalog::AlterStatistics(const AlterStatisticsStmt& stmt) {
    std::string name = JoinName(stmt.stxnames);

    Oid found = 0;
    for (const auto& [oid, data] : cache_) {
        if (data.stxname == name) {
            found = oid;
            break;
        }
    }
    if (found == 0) {
        if (stmt.missing_ok)
            return;
        ereport(LogLevel::kError, "statistics object \"" + name + "\" does not exist");
    }
    cache_[found].stxstattarget = stmt.stxstattarget;
}

void StatisticsCatalog::RemoveStatistics(Oid stxoid) {
    cache_.erase(stxoid);
}

const StatsExtData* StatisticsCatalog::Lookup(Oid stxoid) const {
    auto it = cache_.find(stxoid);
    if (it == cache_.end())
        return nullptr;
    return &it->second;
}

const StatsExtData* StatisticsCatalog::LookupByName(const std::string& name) const {
    for (const auto& [oid, data] : cache_) {
        if (data.stxname == name)
            return &data;
    }
    return nullptr;
}

void StatisticsCatalog::Clear() {
    cache_.clear();
    next_oid_ = 10000;
}

// === StatisticsProvider ===

void StatisticsProvider::Register(StatisticsProvider* provider) {
    ProviderRegistry()[provider->Kind()] = provider;
}

void StatisticsProvider::Unregister(StatisticsProvider* provider) {
    auto& reg = ProviderRegistry();
    auto it = reg.find(provider->Kind());
    if (it != reg.end() && it->second == provider) {
        reg.erase(it);
    }
}

StatisticsProvider* StatisticsProvider::Lookup(StatsExtKind kind) {
    auto& reg = ProviderRegistry();
    auto it = reg.find(kind);
    if (it == reg.end())
        return nullptr;
    return it->second;
}

void StatisticsProvider::ClearRegistry() {
    ProviderRegistry().clear();
}

// === SQL-facing convenience wrappers ===

Oid CreateStatistics(const CreateStatisticsStmt& stmt) {
    return StatisticsCatalog::Instance().CreateStatistics(stmt);
}

void AlterStatistics(const AlterStatisticsStmt& stmt) {
    StatisticsCatalog::Instance().AlterStatistics(stmt);
}

void RemoveStatistics(Oid stxoid) {
    StatisticsCatalog::Instance().RemoveStatistics(stxoid);
}

}  // namespace pgcpp::statistics
