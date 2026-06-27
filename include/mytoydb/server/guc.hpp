// guc.h — Grand Unified Configuration (GUC) loading from postgresql.conf.
//
// Converted from PostgreSQL 15's src/backend/utils/misc/guc.c and guc-file.l.
//
// PostgreSQL's GUC system manages thousands of configuration variables. For
// MyToyDB we implement a minimal subset: parse a postgresql.conf file into a
// key-value map, then apply selected values to ServerConfig.
//
// Supported file format (simplified, matches PostgreSQL's guc-file.l):
//   - Lines starting with '#' (after optional leading whitespace) are comments.
//   - Blank lines are ignored.
//   - Each setting line is `key = value`.
//   - Whitespace around key and value is stripped.
//   - Values may be wrapped in single quotes; the quotes are removed.
//   - Later settings for the same key override earlier ones.
//
// Bool values accept (case-insensitive): on/true/yes/1 and off/false/no/0.
#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace mytoydb::server {

struct ServerConfig;

// Well-known GUC names used by the server.
constexpr const char* kGucPort = "port";
constexpr const char* kGucMaxConnections = "max_connections";
constexpr const char* kGucListenAddresses = "listen_addresses";

// GucConfig — a parsed set of GUC key-value pairs.
class GucConfig {
public:
    GucConfig() = default;

    // Load GUCs from a postgresql.conf-style file.
    // Returns true on success, false if the file could not be opened.
    // Existing entries are preserved (the file is merged in).
    bool LoadFile(const std::string& path);

    // Load GUCs from an in-memory string (same format as a file).
    // Useful for tests. Existing entries are preserved.
    void LoadFromString(const std::string& content);

    // Get a GUC value as a string. Returns default_value if not set.
    std::string GetString(const std::string& key, const std::string& default_value = "") const;

    // Get a GUC value as an int. Returns default_value if not set or invalid.
    int GetInt(const std::string& key, int default_value = 0) const;

    // Get a GUC value as a bool (accepts on/true/yes/1, off/false/no/0,
    // case-insensitive). Returns default_value if not set or unrecognized.
    bool GetBool(const std::string& key, bool default_value = false) const;

    // True if a GUC with the given key is set.
    bool Has(const std::string& key) const;

    // Number of GUCs currently set.
    std::size_t size() const { return values_.size(); }

    // Apply port/max_connections/listen_addresses (if set) to a ServerConfig.
    // Only values present in this GucConfig are applied; others are untouched.
    void ApplyTo(ServerConfig* config) const;

private:
    // Parse a single non-empty, non-comment line into key/value and store it.
    void ParseLine(const std::string& line);

    std::map<std::string, std::string> values_;
};

}  // namespace mytoydb::server
