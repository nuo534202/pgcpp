// fastpath.cpp — Fastpath function-call protocol.
//
// Parses 'F' (FunctionCall) messages, dispatches to a registered handler
// keyed by function OID, and sends a 'V' (FunctionCallResponse) message
// containing the result.
#include "protocol/fastpath.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "protocol/auth.hpp"  // SendErrorResponse
#include "protocol/pqformat.hpp"

namespace pgcpp::protocol {

namespace {

std::mutex& RegistryMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

FunctionRegistry& GetGlobalFunctionRegistry() {
    static FunctionRegistry r;
    return r;
}

void FunctionRegistry::Register(int32_t oid, FunctionHandler handler) {
    std::lock_guard<std::mutex> g(RegistryMutex());
    handlers_[oid] = std::move(handler);
}

FunctionHandler FunctionRegistry::Lookup(int32_t oid) const {
    std::lock_guard<std::mutex> g(RegistryMutex());
    auto it = handlers_.find(oid);
    if (it == handlers_.end())
        return nullptr;
    return it->second;
}

void FunctionRegistry::Unregister(int32_t oid) {
    std::lock_guard<std::mutex> g(RegistryMutex());
    handlers_.erase(oid);
}

void FunctionRegistry::Clear() {
    std::lock_guard<std::mutex> g(RegistryMutex());
    handlers_.clear();
}

int FunctionRegistry::Size() const {
    std::lock_guard<std::mutex> g(RegistryMutex());
    return static_cast<int>(handlers_.size());
}

bool ParseFastpathArgs(MessageReader& reader, std::vector<FastpathArg>& args) {
    int16_t nargs = reader.ReadInt16();
    args.clear();
    args.reserve(nargs);
    for (int16_t i = 0; i < nargs; ++i) {
        FastpathArg a;
        a.format = reader.ReadInt16();
        int32_t len = reader.ReadInt32();
        if (len < 0) {
            a.is_null = true;
        } else {
            a.is_null = false;
            a.data = reader.ReadBytes(static_cast<std::size_t>(len));
        }
        args.push_back(std::move(a));
    }
    return true;
}

Message BuildFunctionCallResponse(const FastpathResult& result) {
    MessageWriter w;
    if (result.is_null) {
        w.WriteInt32(-1);
    } else {
        w.WriteInt32(static_cast<int32_t>(result.data.size()));
        w.WriteBytes(result.data.data(), result.data.size());
    }
    // 'V' (FunctionCallResponse) is not in our MessageType enum; cast the
    // ASCII code directly (MessageType is `enum class : char`).
    Message m = w.BuildMessage(static_cast<MessageType>('V'));
    return m;
}

bool HandleFunctionRequest(const std::string& payload, OutputSink* sink) {
    MessageReader reader(payload);
    int32_t oid = reader.ReadInt32();
    std::vector<FastpathArg> args;
    if (!ParseFastpathArgs(reader, args)) {
        SendErrorResponse(sink, "08P01", "malformed fastpath function call");
        return true;
    }
    auto handler = GetGlobalFunctionRegistry().Lookup(oid);
    if (!handler) {
        SendErrorResponse(sink, "42883",
                          "function with OID " + std::to_string(oid) + " does not exist");
        return true;
    }
    FastpathResult result;
    result = handler(args);
    sink->SendMessage(BuildFunctionCallResponse(result));
    return true;
}

}  // namespace pgcpp::protocol
