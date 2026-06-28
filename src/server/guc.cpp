// guc.cpp — GUC (postgresql.conf) loading implementation.
//
// Converted from PostgreSQL 15's src/backend/utils/misc/guc.c and guc-file.l.
//
// Parses a postgresql.conf-style file into a key-value map. The parser is
// intentionally minimal: it handles comments, blank lines, `key = value`
// lines, optional single-quoted values, and case-insensitive booleans.
#include "pgcpp/server/guc.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <string>

#include "pgcpp/server/postmaster.hpp"

namespace mytoydb::server {

namespace {

// Trim leading and trailing whitespace from a string.
std::string Trim(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

// Lowercase a string (ASCII only).
std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

}  // namespace

void GucConfig::ParseLine(const std::string& line) {
    // Find the first '=' separator.
    std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
        return;  // Not a setting line; ignore.
    }

    std::string key = Trim(line.substr(0, eq));
    std::string value = Trim(line.substr(eq + 1));

    if (key.empty()) {
        return;
    }

    // Strip surrounding single quotes from the value (PostgreSQL convention).
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        value = value.substr(1, value.size() - 2);
    }

    values_[key] = value;
}

void GucConfig::LoadFromString(const std::string& content) {
    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t nl = content.find('\n', pos);
        std::string line;
        if (nl == std::string::npos) {
            line = content.substr(pos);
            pos = content.size();
        } else {
            line = content.substr(pos, nl - pos);
            pos = nl + 1;
        }

        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        ParseLine(trimmed);
    }
}

bool GucConfig::LoadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    LoadFromString(content);
    return true;
}

std::string GucConfig::GetString(const std::string& key, const std::string& default_value) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return default_value;
    }
    return it->second;
}

int GucConfig::GetInt(const std::string& key, int default_value) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return default_value;
    }
    // Use strtol so we can detect non-numeric values without exceptions.
    const std::string& s = it->second;
    const char* start = s.c_str();
    char* end = nullptr;
    long v = std::strtol(start, &end, 10);
    // Require the entire (trimmed) value to be consumed.
    std::string rest = Trim(std::string(end));
    if (end == start || !rest.empty() || v < INT_MIN || v > INT_MAX) {
        return default_value;
    }
    return static_cast<int>(v);
}

bool GucConfig::GetBool(const std::string& key, bool default_value) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return default_value;
    }
    std::string v = ToLower(it->second);
    if (v == "on" || v == "true" || v == "yes" || v == "1") {
        return true;
    }
    if (v == "off" || v == "false" || v == "no" || v == "0") {
        return false;
    }
    return default_value;
}

bool GucConfig::Has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

void GucConfig::ApplyTo(ServerConfig* config) const {
    if (config == nullptr) {
        return;
    }
    if (Has(kGucPort)) {
        int port = GetInt(kGucPort, config->port);
        if (port > 0) {
            config->port = port;
        }
    }
    if (Has(kGucMaxConnections)) {
        int max_conn = GetInt(kGucMaxConnections, config->max_connections);
        if (max_conn > 0) {
            config->max_connections = max_conn;
        }
    }
    if (Has(kGucListenAddresses)) {
        config->listen_addr = GetString(kGucListenAddresses);
    }
}

}  // namespace mytoydb::server
