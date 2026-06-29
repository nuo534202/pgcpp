// ts_types.cpp — tsvector/tsquery implementations.

#include "types/ts_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::types {

using pgcpp::error::LogLevel;
using pgcpp::memory::MemoryContext;
using pgcpp::memory::palloc;

namespace {

char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

}  // namespace

Datum MakeTsVectorDatum(const TsVectorData& data) {
    auto* p = static_cast<TsVectorData*>(palloc(sizeof(TsVectorData)));
    new (p) TsVectorData(data);
    MemoryContext* ctx = pgcpp::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(p, [](void* o) { static_cast<TsVectorData*>(o)->~TsVectorData(); });
    }
    return reinterpret_cast<Datum>(p);
}

Datum MakeTsQueryDatum(const TsQueryData& data) {
    auto* p = static_cast<TsQueryData*>(palloc(sizeof(TsQueryData)));
    new (p) TsQueryData(data);
    MemoryContext* ctx = pgcpp::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(p, [](void* o) { static_cast<TsQueryData*>(o)->~TsQueryData(); });
    }
    return reinterpret_cast<Datum>(p);
}

Datum tsvector_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type tsvector: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    auto* v = static_cast<TsVectorData*>(palloc(sizeof(TsVectorData)));
    new (v) TsVectorData();
    MemoryContext* ctx = pgcpp::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(v, [](void* o) { static_cast<TsVectorData*>(o)->~TsVectorData(); });
    }
    while (it < s.size()) {
        while (it < s.size() && std::isspace(static_cast<unsigned char>(s[it]))) {
            ++it;
        }
        if (it >= s.size()) {
            break;
        }
        std::string lexeme;
        while (it < s.size() && IsWordChar(s[it])) {
            lexeme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[it]))));
            ++it;
        }
        if (lexeme.empty()) {
            ereport(LogLevel::kError, "invalid tsvector literal: \"" + std::string(str) + "\"");
        }
        TsWordEntry entry;
        entry.lexeme = lexeme;
        // Optional ":pos1,pos2" suffix
        if (it < s.size() && s[it] == ':') {
            ++it;
            while (it < s.size() && std::isdigit(static_cast<unsigned char>(s[it]))) {
                int val = 0;
                while (it < s.size() && std::isdigit(static_cast<unsigned char>(s[it]))) {
                    val = val * 10 + (s[it] - '0');
                    ++it;
                }
                entry.positions.push_back(val);
                if (it < s.size() && s[it] == ',') {
                    ++it;
                } else {
                    break;
                }
            }
        }
        v->entries.push_back(entry);
    }
    std::sort(v->entries.begin(), v->entries.end(),
              [](const TsWordEntry& a, const TsWordEntry& b) { return a.lexeme < b.lexeme; });
    return MakeTsVectorDatum(*v);
}

char* tsvector_out(Datum value) {
    const auto* v = DatumGetTsVector(value);
    std::string out;
    for (std::size_t i = 0; i < v->entries.size(); ++i) {
        if (i > 0) {
            out.push_back(' ');
        }
        out += v->entries[i].lexeme;
        if (!v->entries[i].positions.empty()) {
            out.push_back(':');
            for (std::size_t j = 0; j < v->entries[i].positions.size(); ++j) {
                if (j > 0) {
                    out.push_back(',');
                }
                out += std::to_string(v->entries[i].positions[j]);
            }
        }
    }
    return PallocCString(out);
}

Datum tsquery_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type tsquery: NULL");
    }
    // Simplified parser: split on '&' (AND), '|' (OR), '!' (NOT).
    std::string s(str);
    std::string current;
    TsQueryData q{};
    auto it = s.begin();
    while (it != s.end()) {
        char c = *it;
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++it;
            continue;
        }
        if (c == '&' || c == '|') {
            if (!current.empty()) {
                TsQueryNode n;
                n.type = (c == '&') ? TsQueryNodeType::kAnd : TsQueryNodeType::kOr;
                TsQueryNode term;
                term.type = TsQueryNodeType::kTerm;
                term.lexeme = current;
                n.children.push_back(term);
                if (!q.root.children.empty()) {
                    // Flatten a chain of AND/OR.
                    TsQueryNode prev = std::move(q.root);
                    q.root = TsQueryNode{};
                    q.root.type = n.type;
                    q.root.children.push_back(prev);
                    q.root.children.push_back(term);
                } else {
                    q.root = n;
                }
                current.clear();
            }
            ++it;
            continue;
        }
        current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        ++it;
    }
    if (!current.empty()) {
        if (q.root.children.empty()) {
            q.root.type = TsQueryNodeType::kTerm;
            q.root.lexeme = current;
        } else {
            TsQueryNode term;
            term.type = TsQueryNodeType::kTerm;
            term.lexeme = current;
            q.root.children.push_back(term);
        }
    }
    return MakeTsQueryDatum(q);
}

char* tsquery_out(Datum value) {
    const auto* q = DatumGetTsQuery(value);
    std::function<std::string(const TsQueryNode&)> emit = [&](const TsQueryNode& n) -> std::string {
        switch (n.type) {
            case TsQueryNodeType::kTerm:
                return n.lexeme;
            case TsQueryNodeType::kAnd: {
                std::string out;
                for (std::size_t i = 0; i < n.children.size(); ++i) {
                    if (i > 0) {
                        out += " & ";
                    }
                    out += emit(n.children[i]);
                }
                return out;
            }
            case TsQueryNodeType::kOr: {
                std::string out;
                for (std::size_t i = 0; i < n.children.size(); ++i) {
                    if (i > 0) {
                        out += " | ";
                    }
                    out += emit(n.children[i]);
                }
                return out;
            }
            case TsQueryNodeType::kNot:
                return "!" + (n.children.empty() ? std::string() : emit(n.children[0]));
        }
        return std::string();
    };
    return PallocCString(emit(q->root));
}

Datum ts_match(Datum tsvector, Datum tsquery) {
    const auto* v = DatumGetTsVector(tsvector);
    const auto* q = DatumGetTsQuery(tsquery);
    std::set<std::string> lexemes;
    for (const auto& e : v->entries) {
        lexemes.insert(e.lexeme);
    }
    // Walk the query tree; AND/OR/TERM.
    std::function<bool(const TsQueryNode&)> match = [&](const TsQueryNode& n) -> bool {
        switch (n.type) {
            case TsQueryNodeType::kTerm:
                return lexemes.count(n.lexeme) > 0;
            case TsQueryNodeType::kAnd:
                for (const auto& c : n.children) {
                    if (!match(c)) {
                        return false;
                    }
                }
                return true;
            case TsQueryNodeType::kOr:
                for (const auto& c : n.children) {
                    if (match(c)) {
                        return true;
                    }
                }
                return false;
            case TsQueryNodeType::kNot:
                return n.children.empty() ? false : !match(n.children[0]);
        }
        return false;
    };
    return BoolGetDatum(match(q->root));
}

}  // namespace pgcpp::types
