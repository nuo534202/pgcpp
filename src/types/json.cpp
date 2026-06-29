// json.cpp — json / jsonb implementations.
//
// A small recursive-descent parser/builder for JSON values. The same tree is
// used for both `json` and `jsonb` types; only display differs slightly.

#include "types/json.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::types {

using pgcpp::error::LogLevel;
using pgcpp::memory::palloc;

// Forward declaration — defined below.
void FreeJsonValue(JsonValue* v);

namespace {

char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

struct Parser {
    std::string_view src;
    std::size_t pos = 0;

    explicit Parser(std::string_view s) : src(s) {}

    void SkipWs() {
        while (pos < src.size() &&
               (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r')) {
            ++pos;
        }
    }

    char Peek() {
        SkipWs();
        return pos < src.size() ? src[pos] : '\0';
    }

    char Get() {
        SkipWs();
        return pos < src.size() ? src[pos++] : '\0';
    }

    JsonValue* ParseValue() {
        SkipWs();
        if (pos >= src.size()) {
            ereport(LogLevel::kError, "unexpected end of JSON input");
        }
        char c = src[pos];
        if (c == '"') {
            return ParseString();
        }
        if (c == '{') {
            return ParseObject();
        }
        if (c == '[') {
            return ParseArray();
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseNumber();
        }
        if (c == 't' || c == 'f') {
            return ParseBool();
        }
        if (c == 'n') {
            return ParseNull();
        }
        ereport(LogLevel::kError, std::string("invalid character in JSON: '") + c + "'");
        return nullptr;  // unreachable — ereport longjmps
    }

    JsonValue* ParseString() {
        auto* v = AllocJsonValue();
        v->type = JsonType::kString;
        ++pos;  // skip opening quote
        while (pos < src.size()) {
            char c = src[pos];
            if (c == '\\') {
                ++pos;
                if (pos >= src.size()) {
                    ereport(LogLevel::kError, "invalid escape in JSON string");
                }
                char esc = src[pos++];
                switch (esc) {
                    case '"':
                        v->string_val.push_back('"');
                        break;
                    case '\\':
                        v->string_val.push_back('\\');
                        break;
                    case '/':
                        v->string_val.push_back('/');
                        break;
                    case 'b':
                        v->string_val.push_back('\b');
                        break;
                    case 'f':
                        v->string_val.push_back('\f');
                        break;
                    case 'n':
                        v->string_val.push_back('\n');
                        break;
                    case 'r':
                        v->string_val.push_back('\r');
                        break;
                    case 't':
                        v->string_val.push_back('\t');
                        break;
                    case 'u': {
                        // We support \uXXXX -> UTF-8 encoding.
                        if (pos + 4 > src.size()) {
                            ereport(LogLevel::kError, "invalid \\u escape in JSON");
                        }
                        int cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src[pos++];
                            int digit = 0;
                            if (h >= '0' && h <= '9') {
                                digit = h - '0';
                            } else if (h >= 'a' && h <= 'f') {
                                digit = h - 'a' + 10;
                            } else if (h >= 'A' && h <= 'F') {
                                digit = h - 'A' + 10;
                            } else {
                                ereport(LogLevel::kError, "invalid hex digit in \\u escape");
                            }
                            cp = cp * 16 + digit;
                        }
                        if (cp < 0x80) {
                            v->string_val.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            v->string_val.push_back(static_cast<char>(0xc0 | (cp >> 6)));
                            v->string_val.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                        } else {
                            v->string_val.push_back(static_cast<char>(0xe0 | (cp >> 12)));
                            v->string_val.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
                            v->string_val.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                        }
                        break;
                    }
                    default:
                        ereport(LogLevel::kError, std::string("invalid escape '\\") + esc + "'");
                }
                continue;
            }
            if (c == '"') {
                ++pos;
                return v;
            }
            v->string_val.push_back(c);
            ++pos;
        }
        ereport(LogLevel::kError, "unterminated string in JSON");
        return nullptr;  // unreachable — ereport longjmps
    }

    JsonValue* ParseNumber() {
        auto* v = AllocJsonValue();
        v->type = JsonType::kNumber;
        std::size_t start = pos;
        if (pos < src.size() && src[pos] == '-') {
            ++pos;
        }
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
        if (pos < src.size() && src[pos] == '.') {
            ++pos;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
                ++pos;
            }
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            ++pos;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
                ++pos;
            }
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
                ++pos;
            }
        }
        std::string num_str(src.substr(start, pos - start));
        // Validate by re-parsing with strtod; we treat errno==ERANGE as error.
        errno = 0;
        char* end = nullptr;
        double d = std::strtod(num_str.c_str(), &end);
        if (errno == ERANGE || end == num_str.c_str() || *end != '\0') {
            ereport(LogLevel::kError, "invalid JSON number: \"" + num_str + "\"");
        }
        v->number_val = d;
        return v;
    }

    JsonValue* ParseBool() {
        if (src.compare(pos, 4, "true") == 0) {
            pos += 4;
            auto* v = AllocJsonValue();
            v->type = JsonType::kBool;
            v->bool_val = true;
            return v;
        }
        if (src.compare(pos, 5, "false") == 0) {
            pos += 5;
            auto* v = AllocJsonValue();
            v->type = JsonType::kBool;
            v->bool_val = false;
            return v;
        }
        ereport(LogLevel::kError, "invalid JSON literal");
        return nullptr;  // unreachable — ereport longjmps
    }

    JsonValue* ParseNull() {
        if (src.compare(pos, 4, "null") == 0) {
            pos += 4;
            auto* v = AllocJsonValue();
            v->type = JsonType::kNull;
            return v;
        }
        ereport(LogLevel::kError, "invalid JSON literal");
        return nullptr;  // unreachable — ereport longjmps
    }

    JsonValue* ParseArray() {
        auto* v = AllocJsonValue();
        v->type = JsonType::kArray;
        ++pos;  // skip '['
        SkipWs();
        if (pos < src.size() && src[pos] == ']') {
            ++pos;
            return v;
        }
        while (true) {
            JsonValue* elem = ParseValue();
            v->array_val.push_back(elem);
            SkipWs();
            if (pos >= src.size()) {
                ereport(LogLevel::kError, "unterminated array in JSON");
            }
            if (src[pos] == ',') {
                ++pos;
                continue;
            }
            if (src[pos] == ']') {
                ++pos;
                return v;
            }
            ereport(LogLevel::kError, "expected ',' or ']' in JSON array");
        }
    }

    JsonValue* ParseObject() {
        auto* v = AllocJsonValue();
        v->type = JsonType::kObject;
        ++pos;  // skip '{'
        SkipWs();
        if (pos < src.size() && src[pos] == '}') {
            ++pos;
            return v;
        }
        while (true) {
            SkipWs();
            if (pos >= src.size() || src[pos] != '"') {
                ereport(LogLevel::kError, "expected string key in JSON object");
            }
            JsonValue* key = ParseString();
            SkipWs();
            if (pos >= src.size() || src[pos] != ':') {
                ereport(LogLevel::kError, "expected ':' after key in JSON object");
            }
            ++pos;
            JsonValue* value = ParseValue();
            v->object_val.emplace_back(key->string_val, value);
            SkipWs();
            if (pos >= src.size()) {
                ereport(LogLevel::kError, "unterminated object in JSON");
            }
            if (src[pos] == ',') {
                ++pos;
                continue;
            }
            if (src[pos] == '}') {
                ++pos;
                return v;
            }
            ereport(LogLevel::kError, "expected ',' or '}' in JSON object");
        }
    }
};

void EmitString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

void EmitNumber(std::string& out, double v) {
    char buf[64];
    if (v == std::floor(v) && v >= -1e15 && v <= 1e15) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%.17g", v);
    }
    out += buf;
}

void EmitValue(std::string& out, const JsonValue* v) {
    switch (v->type) {
        case JsonType::kNull:
            out += "null";
            return;
        case JsonType::kBool:
            out += v->bool_val ? "true" : "false";
            return;
        case JsonType::kNumber:
            EmitNumber(out, v->number_val);
            return;
        case JsonType::kString:
            EmitString(out, v->string_val);
            return;
        case JsonType::kArray:
            out.push_back('[');
            for (std::size_t i = 0; i < v->array_val.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                EmitValue(out, v->array_val[i]);
            }
            out.push_back(']');
            return;
        case JsonType::kObject:
            out.push_back('{');
            for (std::size_t i = 0; i < v->object_val.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                EmitString(out, v->object_val[i].first);
                out.push_back(':');
                EmitValue(out, v->object_val[i].second);
            }
            out.push_back('}');
            return;
    }
}

}  // namespace

JsonValue* AllocJsonValue() {
    auto* v = static_cast<JsonValue*>(palloc(sizeof(JsonValue)));
    new (v) JsonValue();
    pgcpp::memory::MemoryContext* ctx = pgcpp::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        // Each child JsonValue is allocated via AllocJsonValue and registers
        // its own destructor; we therefore only destroy this node's own
        // std::string / std::vector members here. Recursively freeing
        // children would double-destruct them when the MemoryContext later
        // walks its destructor list.
        ctx->RegisterDestructor(v, [](void* p) {
            JsonValue* jv = static_cast<JsonValue*>(p);
            jv->~JsonValue();
        });
    }
    return v;
}

void FreeJsonValue(JsonValue* v) {
    if (v == nullptr) {
        return;
    }
    for (JsonValue* child : v->array_val) {
        FreeJsonValue(child);
    }
    for (auto& kv : v->object_val) {
        FreeJsonValue(kv.second);
    }
    v->~JsonValue();
}

Datum MakeJsonDatum(JsonValue* value) {
    return reinterpret_cast<Datum>(value);
}

Datum json_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type json: NULL");
    }
    Parser p(str);
    JsonValue* v = p.ParseValue();
    p.SkipWs();
    if (p.pos != p.src.size()) {
        ereport(LogLevel::kError, "trailing garbage in JSON input");
    }
    return MakeJsonDatum(v);
}

char* json_out(Datum value) {
    const auto* v = DatumGetJson(value);
    std::string out;
    EmitValue(out, v);
    return PallocCString(out);
}

Datum jsonb_in(const char* str) {
    // Same parser; the only difference is display format.
    return json_in(str);
}

char* jsonb_out(Datum value) {
    return json_out(value);
}

int json_cmp(Datum a, Datum b) {
    // Compare serialized forms as a simple total ordering.
    char* sa = json_out(a);
    char* sb = json_out(b);
    int cmp = std::strcmp(sa, sb);
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

Datum json_eq(Datum a, Datum b) {
    return BoolGetDatum(json_cmp(a, b) == 0);
}

}  // namespace pgcpp::types
