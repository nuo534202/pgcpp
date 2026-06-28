// xml.cpp — XML type implementation (PostgreSQL utils/adt/xml.c).

#include "pgcpp/types/xml.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <stack>
#include <string>
#include <string_view>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/builtins.hpp"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::palloc;

Datum xml_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type xml: NULL");
    }
    return text_in(str);
}

char* xml_out(Datum value) {
    return text_out(value);
}

Datum xml_validate(Datum value) {
    // Get the underlying text.
    const char* text = DatumGetTextP(value);
    int len = VARSIZE_DATA(text);
    const char* data = VARDATA(text);
    // Stack-based tag matching. Each entry is the tag name we're inside.
    std::stack<std::string> tags;
    std::size_t i = 0;
    std::size_t open_count = 0;
    while (i < static_cast<std::size_t>(len)) {
        if (data[i] != '<') {
            ++i;
            continue;
        }
        ++i;
        if (i >= static_cast<std::size_t>(len)) {
            return BoolGetDatum(false);
        }
        if (data[i] == '?') {
            // Processing instruction: skip to '?>'.
            while (i + 1 < static_cast<std::size_t>(len) &&
                   !(data[i] == '?' && data[i + 1] == '>')) {
                ++i;
            }
            i += 2;
            continue;
        }
        if (data[i] == '!') {
            // Comment or CDATA: skip to '>'.
            while (i < static_cast<std::size_t>(len) && data[i] != '>') {
                ++i;
            }
            ++i;
            continue;
        }
        bool closing = false;
        if (data[i] == '/') {
            closing = true;
            ++i;
        }
        // Tag name.
        std::string name;
        while (i < static_cast<std::size_t>(len) &&
               !std::isspace(static_cast<unsigned char>(data[i])) && data[i] != '>' &&
               data[i] != '/') {
            name.push_back(data[i]);
            ++i;
        }
        // Skip attributes.
        while (i < static_cast<std::size_t>(len) && data[i] != '>' && data[i] != '/') {
            ++i;
        }
        bool self_closing = false;
        if (i < static_cast<std::size_t>(len) && data[i] == '/') {
            self_closing = true;
            ++i;
        }
        if (i >= static_cast<std::size_t>(len) || data[i] != '>') {
            return BoolGetDatum(false);
        }
        ++i;
        if (closing) {
            if (tags.empty() || tags.top() != name) {
                return BoolGetDatum(false);
            }
            tags.pop();
        } else if (!self_closing) {
            if (tags.empty()) {
                ++open_count;
            }
            tags.push(name);
        }
    }
    return BoolGetDatum(open_count == 1 && tags.empty());
}

Datum xml_concat(Datum a, Datum b) {
    return text_concat(a, b);
}

Datum xpath_exists(Datum xml, Datum xpath) {
    // Simplified: check whether the xpath string is a substring of the XML.
    const char* xml_text = DatumGetTextP(xml);
    const char* xpath_text = DatumGetTextP(xpath);
    int xml_len = VARSIZE_DATA(xml_text);
    int xpath_len = VARSIZE_DATA(xpath_text);
    const char* xml_data = VARDATA(xml_text);
    const char* xpath_data = VARDATA(xpath_text);
    if (xpath_len == 0) {
        return BoolGetDatum(true);
    }
    if (xpath_len > xml_len) {
        return BoolGetDatum(false);
    }
    for (int i = 0; i <= xml_len - xpath_len; ++i) {
        if (std::memcmp(xml_data + i, xpath_data, static_cast<std::size_t>(xpath_len)) == 0) {
            return BoolGetDatum(true);
        }
    }
    return BoolGetDatum(false);
}

}  // namespace mytoydb::types
