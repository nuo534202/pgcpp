#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace pgcpp::containers {

// StringInfo — a dynamic string buffer.
// Faithful conversion of PostgreSQL's StringInfoData.
// Backed by std::string instead of manual char*/malloc.
class StringInfo {
public:
    StringInfo() = default;
    explicit StringInfo(std::string_view initial) : data_(initial) {}

    // Initialize (PostgreSQL initStringInfo). With std::string backing,
    // this is a no-op but kept for API compatibility.
    void Init() {}

    // Append formatted string (PostgreSQL appendStringInfo).
    // Uses printf-style formatting internally.
    void AppendPrintf(const char* fmt, ...);

    // Append a string (PostgreSQL appendStringInfoString)
    void AppendString(std::string_view str);
    // Append a single char (PostgreSQL appendStringInfoChar)
    void AppendChar(char c);
    // Append N bytes (PostgreSQL appendBinaryStringInfo)
    void AppendBinary(const char* data, std::size_t len);

    // Reset to empty (PostgreSQL resetStringInfo)
    void Reset() { data_.clear(); }

    // Accessors
    const char* Data() const { return data_.c_str(); }
    char* MutableData() { return data_.data(); }
    std::size_t Length() const { return data_.size(); }
    std::size_t Capacity() const { return data_.capacity(); }
    bool IsEmpty() const { return data_.empty(); }

    // Get the underlying std::string (C++ convenience)
    const std::string& Str() const { return data_; }
    std::string& Str() { return data_; }

private:
    std::string data_;
};

// PostgreSQL-compatible API (lowercase).
using StringInfoData = StringInfo;

// Create a new StringInfo (allocated via palloc).
StringInfo* makeStringInfo();
void initStringInfo(StringInfo* si);
void appendStringInfo(StringInfo* si, const char* fmt, ...);
void appendStringInfoString(StringInfo* si, std::string_view str);
void appendStringInfoChar(StringInfo* si, char c);
void appendBinaryStringInfo(StringInfo* si, const char* data, std::size_t len);
void resetStringInfo(StringInfo* si);
const char* Data(const StringInfo* si);
int Length(const StringInfo* si);

}  // namespace pgcpp::containers
