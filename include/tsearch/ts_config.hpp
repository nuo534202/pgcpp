#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tsearch/dict.hpp"

namespace pgcpp::tsearch {

// ---------------------------------------------------------------------------
// TsearchConfigRegistry — runtime registry of text-search configurations
// (PostgreSQL's tsconfig cache, src/backend/utils/cache/ts_cache.c).
//
// Each named configuration (e.g. "english", "simple") is bound to a factory
// that builds the dictionary chain applied to each token during to_tsvector /
// to_tsquery. The chain is an ordered list of dictionaries; a token is passed
// through each dictionary in turn, and the first dictionary that produces a
// non-stop lexeme wins (matching PostgreSQL's lexize behavior).
//
// This registry holds the built-in configurations and provides a lookup API
// used by ToTsVector / ToTsQuery. It is separate from the main Catalog class
// because tsearch configurations are static (bootstrap-defined) in pgcpp's
// single-process model.
// ---------------------------------------------------------------------------

class TsearchConfigRegistry {
public:
    // A factory function that builds a fresh dictionary chain for a config.
    // Returns owning unique_ptrs so each pipeline invocation gets its own
    // (stateless) dictionary objects.
    using DictChain = std::vector<std::unique_ptr<IDictionary>>;
    using DictFactory = std::function<DictChain()>;

    TsearchConfigRegistry();
    ~TsearchConfigRegistry();

    TsearchConfigRegistry(const TsearchConfigRegistry&) = delete;
    TsearchConfigRegistry& operator=(const TsearchConfigRegistry&) = delete;

    // Register a configuration under `name`. Replaces any existing entry.
    void Register(std::string name, DictFactory factory);

    // Build the dictionary chain for `config`. Returns an empty chain if the
    // config is unknown (callers should treat this as a no-op pipeline).
    DictChain BuildChain(std::string_view config) const;

    // True if `config` is a registered configuration name.
    bool HasConfig(std::string_view config) const;

private:
    struct Entry {
        std::string name;
        DictFactory factory;
    };
    std::vector<Entry> entries_;
};

// Global registry accessor. Returns the process-wide instance, populating it
// with built-in configurations ("simple", "english") on first use.
TsearchConfigRegistry& GetTsearchConfigRegistry();

// Populate `reg` with the built-in PostgreSQL configurations. Called once at
// startup; idempotent.
void RegisterBuiltinTsearchConfigs(TsearchConfigRegistry& reg);

// Apply a dictionary chain to a single token. The token is fed through each
// dictionary in order; each dictionary's output text becomes the next one's
// input. If any dictionary marks the token as a stop word (is_stop=true), the
// token is dropped and std::nullopt is returned. Otherwise the final lexeme
// (after all transformations) is returned.
std::optional<Lexeme> ApplyDictionaryChain(const TsearchConfigRegistry::DictChain& chain,
                                           std::string_view token);

}  // namespace pgcpp::tsearch
