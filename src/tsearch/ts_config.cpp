// ts_config.cpp — text-search configuration registry implementation.
//
// Provides the built-in "simple" and "english" configurations that mirror
// PostgreSQL's default text-search setups. Each configuration is a factory
// that builds a fresh dictionary chain on demand:
//
//   "simple"  — SimpleDict only (lowercase, no stemming, no stop words).
//   "english" — StopWordsDict + SnowballDict (stop-word removal + real
//               Porter/Snowball stemming).

#include "tsearch/ts_config.hpp"

#include <utility>

#include "tsearch/dict.hpp"
#include "tsearch/snowball.hpp"

namespace pgcpp::tsearch {

TsearchConfigRegistry::TsearchConfigRegistry() = default;
TsearchConfigRegistry::~TsearchConfigRegistry() = default;

void TsearchConfigRegistry::Register(std::string name, DictFactory factory) {
    for (auto& entry : entries_) {
        if (entry.name == name) {
            entry.factory = std::move(factory);
            return;
        }
    }
    entries_.push_back({std::move(name), std::move(factory)});
}

TsearchConfigRegistry::DictChain TsearchConfigRegistry::BuildChain(std::string_view config) const {
    for (const auto& entry : entries_) {
        if (entry.name == config) {
            return entry.factory();
        }
    }
    return {};
}

bool TsearchConfigRegistry::HasConfig(std::string_view config) const {
    for (const auto& entry : entries_) {
        if (entry.name == config)
            return true;
    }
    return false;
}

void RegisterBuiltinTsearchConfigs(TsearchConfigRegistry& reg) {
    // "simple": lowercase only (SimpleDict with no stop-word removal).
    reg.Register("simple", []() -> TsearchConfigRegistry::DictChain {
        TsearchConfigRegistry::DictChain chain;
        chain.push_back(std::make_unique<SimpleDict>());
        return chain;
    });

    // "english": stop-word removal + Snowball (Porter) stemming.
    reg.Register("english", []() -> TsearchConfigRegistry::DictChain {
        TsearchConfigRegistry::DictChain chain;
        chain.push_back(std::make_unique<StopWordsDict>());
        chain.push_back(std::make_unique<SnowballDict>());
        return chain;
    });
}

namespace {

TsearchConfigRegistry& GlobalRegistry() {
    static TsearchConfigRegistry reg;
    static bool initialized = false;
    if (!initialized) {
        RegisterBuiltinTsearchConfigs(reg);
        initialized = true;
    }
    return reg;
}

}  // namespace

TsearchConfigRegistry& GetTsearchConfigRegistry() {
    return GlobalRegistry();
}

std::optional<Lexeme> ApplyDictionaryChain(const TsearchConfigRegistry::DictChain& chain,
                                           std::string_view token) {
    Lexeme current{std::string(token), false};
    for (const auto& dict : chain) {
        Lexeme next = dict->Lexicalize(current.text);
        if (next.is_stop) {
            return std::nullopt;  // filtered as a stop word
        }
        current = std::move(next);
    }
    return current;
}

}  // namespace pgcpp::tsearch
