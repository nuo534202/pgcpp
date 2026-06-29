// fastpath.h — Fastpath function-call protocol (fastpath.c).
//
// Converted from PostgreSQL 15's src/backend/tcop/fastpath.c.
//
// The fastpath protocol is an older mechanism for invoking built-in
// functions by OID directly, bypassing SQL parsing. The frontend sends a
// 'F' message (function call) containing:
//   - int32: function OID
//   - int16: number of arguments
//   - per argument: int16 format (0=text, 1=binary) + int32 length + bytes
//
// The backend looks up the function in pg_proc, coerces the arguments to
// the declared types, invokes the function, and returns a single result
// value as a 'V' (FunctionCallResponse) message. On error it sends an
// ErrorResponse ('E').
//
// pgcpp implements the message parsing and result encoding; the actual
// function invocation is dispatched through a registry (FunctionRegistry)
// that maps OID -> std::function. This avoids a hard dependency on the
// catalog and lets tests register stub functions.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "protocol/pqformat.hpp"

namespace pgcpp::protocol {

// FastpathArg — one argument to a fastpath function call.
struct FastpathArg {
    int16_t format = 0;  // 0 = text, 1 = binary
    std::string data;    // raw bytes (already length-prefixed in the wire format)
    bool is_null = false;
};

// FastpathResult — the result of a fastpath function call.
struct FastpathResult {
    int16_t format = 0;  // 0 = text, 1 = binary
    std::string data;
    bool is_null = false;
};

// FunctionHandler — signature for a registered fastpath function.
// Takes the argument list; returns the result. May set is_null.
using FunctionHandler = std::function<FastpathResult(const std::vector<FastpathArg>&)>;

// FunctionRegistry — maps function OID -> handler. Per-process (the backend
// registers builtins at startup; tests register stubs).
class FunctionRegistry {
public:
    // Register a handler for `oid`. Overwrites any existing handler.
    void Register(int32_t oid, FunctionHandler handler);

    // Lookup the handler for `oid`. Returns nullptr if not registered.
    FunctionHandler Lookup(int32_t oid) const;

    // Remove the handler for `oid` (if any).
    void Unregister(int32_t oid);

    // Clear all handlers.
    void Clear();

    // Number of registered handlers (for testing).
    int Size() const;

private:
    std::map<int32_t, FunctionHandler> handlers_;
};

// GetGlobalFunctionRegistry — the process-wide registry (used by HandleFunctionRequest).
FunctionRegistry& GetGlobalFunctionRegistry();

// HandleFunctionRequest — process a 'F' (fastpath function call) message.
// `payload` is the message body (everything after the type byte and length).
// Sends a 'V' (FunctionCallResponse) message on success, or 'E' (ErrorResponse)
// on error. Returns true if a response was sent.
bool HandleFunctionRequest(const std::string& payload, OutputSink* sink);

// ParseFastpathArgs — parse the per-argument portion of an 'F' message.
// Returns true on success; false on malformed input.
bool ParseFastpathArgs(MessageReader& reader, std::vector<FastpathArg>& args);

// BuildFunctionCallResponse — build a 'V' message from a result.
Message BuildFunctionCallResponse(const FastpathResult& result);

}  // namespace pgcpp::protocol
