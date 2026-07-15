// pl_handler.cpp — PL handler registry implementation.
//
// Maintains a simple process-wide vector of PlHandler pointers. Registration
// happens at server startup (each PL's module registers itself); lookups
// happen in fmgr_info and ProcessUtility (for DO blocks).
#include "pl/pl_handler.hpp"

#include <algorithm>
#include <vector>

namespace pgcpp::pl {

namespace {

// Process-wide registry of PL handlers. Pointers are not owned (handlers are
// typically file-scope statics) and must remain valid for the process
// lifetime.
std::vector<const PlHandler*>& HandlerRegistry() {
    static std::vector<const PlHandler*> registry;
    return registry;
}

}  // namespace

void RegisterPlHandler(const PlHandler* handler) {
    if (handler == nullptr)
        return;
    auto& registry = HandlerRegistry();
    // Replace if a handler for this language_oid already exists.
    for (auto*& slot : registry) {
        if (slot != nullptr && slot->language_oid == handler->language_oid) {
            slot = handler;
            return;
        }
    }
    registry.push_back(handler);
}

const PlHandler* LookupPlHandler(pgcpp::catalog::Oid language_oid) {
    const auto& registry = HandlerRegistry();
    for (const PlHandler* h : registry) {
        if (h != nullptr && h->language_oid == language_oid)
            return h;
    }
    return nullptr;
}

const PlHandler* LookupPlHandlerByName(const std::string& name) {
    const auto& registry = HandlerRegistry();
    for (const PlHandler* h : registry) {
        if (h != nullptr && h->language_name == name)
            return h;
    }
    return nullptr;
}

void ClearPlHandlers() {
    HandlerRegistry().clear();
}

}  // namespace pgcpp::pl
