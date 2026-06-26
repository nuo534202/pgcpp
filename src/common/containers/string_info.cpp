#include "mytoydb/common/containers/string_info.h"

#include <cstdarg>
#include <cstdio>
#include <new>
#include <vector>

#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/memory/memory_context.h"

namespace mytoydb::containers {
using mytoydb::nodes::makePallocNode;

// ---------------------------------------------------------------------------
// StringInfo method implementations
// ---------------------------------------------------------------------------

void StringInfo::AppendPrintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // First, determine the required buffer size.
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed < 0) {
        va_end(args);
        return;
    }

    // Format into a temporary buffer, then append.
    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
    va_end(args);

    data_.append(buffer.data(), static_cast<std::size_t>(needed));
}

void StringInfo::AppendString(std::string_view str) {
    data_.append(str);
}

void StringInfo::AppendChar(char c) {
    data_.push_back(c);
}

void StringInfo::AppendBinary(const char* data, std::size_t len) {
    data_.append(data, len);
}

// ---------------------------------------------------------------------------
// PostgreSQL-compatible API (lowercase). These operate on StringInfo*
// allocated via palloc.
// ---------------------------------------------------------------------------

StringInfo* makeStringInfo() {
    return makePallocNode<StringInfo>();
}

void initStringInfo(StringInfo* si) {
    if (si != nullptr) {
        si->Init();
    }
}

void appendStringInfo(StringInfo* si, const char* fmt, ...) {
    if (si == nullptr) {
        return;
    }

    va_list args;
    va_start(args, fmt);

    va_list args_copy;
    va_copy(args_copy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed < 0) {
        va_end(args);
        return;
    }

    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
    va_end(args);

    si->Str().append(buffer.data(), static_cast<std::size_t>(needed));
}

void appendStringInfoString(StringInfo* si, std::string_view str) {
    if (si != nullptr) {
        si->AppendString(str);
    }
}

void appendStringInfoChar(StringInfo* si, char c) {
    if (si != nullptr) {
        si->AppendChar(c);
    }
}

void appendBinaryStringInfo(StringInfo* si, const char* data, std::size_t len) {
    if (si != nullptr) {
        si->AppendBinary(data, len);
    }
}

void resetStringInfo(StringInfo* si) {
    if (si != nullptr) {
        si->Reset();
    }
}

const char* Data(const StringInfo* si) {
    if (si == nullptr) {
        return nullptr;
    }
    return si->Data();
}

int Length(const StringInfo* si) {
    if (si == nullptr) {
        return 0;
    }
    return static_cast<int>(si->Length());
}

}  // namespace mytoydb::containers
