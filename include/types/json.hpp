#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// JSON / JSONB (PostgreSQL utils/adt/json.c, jsonb.c).
//
// A small in-memory JSON value tree. Both `json` and `jsonb` types share the
// same in-memory representation; the differences are in display form and
// indexing.
//
// Storage: a palloc'd JsonValue; Datum is a pointer.
// ---------------------------------------------------------------------------

enum class JsonType : uint8_t {
    kNull,
    kBool,
    kNumber,
    kString,
    kArray,
    kObject,
};

struct JsonValue;

using JsonArray = std::vector<JsonValue*>;
using JsonObject = std::vector<std::pair<std::string, JsonValue*>>;

struct JsonValue {
    JsonType type;
    bool bool_val = false;
    double number_val = 0.0;
    std::string string_val;
    JsonArray array_val;
    JsonObject object_val;
};

// --- json ---
Datum json_in(const char* str);
char* json_out(Datum value);

// --- jsonb ---
Datum jsonb_in(const char* str);
char* jsonb_out(Datum value);

int json_cmp(Datum a, Datum b);
Datum json_eq(Datum a, Datum b);

// Helper: build a JsonValue datum (allocates via palloc).
Datum MakeJsonDatum(JsonValue* value);
inline JsonValue* DatumGetJson(Datum x) {
    return reinterpret_cast<JsonValue*>(x);
}

// Allocate a JsonValue (zero-initialised to kNull).
JsonValue* AllocJsonValue();

// Free a JsonValue tree (recursive). Use only outside MemoryContext delete.
void FreeJsonValue(JsonValue* v);

}  // namespace pgcpp::types
