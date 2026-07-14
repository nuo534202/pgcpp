// jit.cpp — JIT expression compilation provider framework.
//
// Converted from PostgreSQL 15's src/backend/jit/jit.c.
//
// Implements the JIT provider dispatch, GUC variables, expression cache,
// and the default interpreter provider (no-op).
#include "jit/jit.hpp"

#include <cstring>
#include <unordered_map>

#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/primnodes.hpp"

namespace pgcpp::jit {

using pgcpp::parser::Node;

// ---------------------------------------------------------------------------
// GUC variables (module-level)
// ---------------------------------------------------------------------------

namespace {

// GUC: jit — enable/disable JIT compilation (default: off in pgcpp, since
// the LLVM provider is not built by default).
bool g_jit_enabled = false;

// GUC: jit_above_cost — minimum expression cost to trigger JIT compilation.
// 0 means "always attempt compilation when JIT is enabled".
int g_jit_above_cost = 100000;

// GUC: jit_inline_above_cost — cost threshold for inlining function calls.
int g_jit_inline_above_cost = 500000;

// GUC: jit_optimize_above_cost — cost threshold for running LLVM optimizations.
int g_jit_optimize_above_cost = 500000;

// The active JIT provider (default: interpreter).
const JitProvider* g_active_provider = nullptr;

// JIT compilation statistics.
JitStats g_stats;

// ---------------------------------------------------------------------------
// JIT expression cache
//
// Maps expression Node* to JitContext*. A nullptr value means compilation
// was attempted and failed (don't retry). The cache uses Node* pointers as
// keys — these are valid for the lifetime of a query's plan tree.
// ---------------------------------------------------------------------------

// Cache entry: stores the JitContext* (or nullptr for "compilation failed").
// We need a wrapper to distinguish "not in cache" from "cached as nullptr".
struct CacheEntry {
    JitContext* ctx = nullptr;
    bool compiled = false;  // true if compilation was attempted
};

std::unordered_map<Node*, CacheEntry>& GetCache() {
    // Function-local static: initialized on first use, destroyed at program exit.
    static std::unordered_map<Node*, CacheEntry> cache;
    return cache;
}

// ---------------------------------------------------------------------------
// Interpreter provider (default, no-op)
//
// The interpreter provider performs no compilation. compile_expr always
// returns nullptr, causing the executor to fall back to the tree-walking
// interpreter in exec_expr.cpp.
// ---------------------------------------------------------------------------

JitContext* InterpreterCompileExpr(Node* /*expr*/) {
    // The interpreter provider never compiles — always return nullptr.
    return nullptr;
}

void InterpreterReleaseContext(JitContext* ctx) {
    // Nothing to release for the interpreter provider.
    if (ctx != nullptr) {
        // The JitContext itself was allocated via palloc; free it.
        // But we don't have palloc here — the provider that allocated it
        // should free it. For the interpreter, ctx should always be nullptr.
    }
}

const char* InterpreterProviderName() {
    return "interpreter";
}

const JitProvider kInterpreterProvider = {
    InterpreterCompileExpr,
    InterpreterReleaseContext,
    InterpreterProviderName,
};

}  // namespace

// ---------------------------------------------------------------------------
// Initialization and provider management
// ---------------------------------------------------------------------------

void InitJit() {
    g_jit_enabled = false;
    g_jit_above_cost = 100000;
    g_jit_inline_above_cost = 500000;
    g_jit_optimize_above_cost = 500000;
    g_active_provider = &kInterpreterProvider;
    g_stats = JitStats{};
    GetCache().clear();
}

void SetJitProvider(const JitProvider* provider) {
    g_active_provider = (provider != nullptr) ? provider : &kInterpreterProvider;
}

const JitProvider* GetJitProvider() {
    if (g_active_provider == nullptr) {
        g_active_provider = &kInterpreterProvider;
    }
    return g_active_provider;
}

const JitProvider* GetInterpreterProvider() {
    return &kInterpreterProvider;
}

// ---------------------------------------------------------------------------
// Compilation and evaluation
// ---------------------------------------------------------------------------

JitContext* JitCompileExpr(Node* expr) {
    g_stats.compile_attempts++;

    if (!IsJitEnabled()) {
        return nullptr;
    }

    const JitProvider* provider = GetJitProvider();
    if (provider == nullptr || provider->compile_expr == nullptr) {
        return nullptr;
    }

    JitContext* ctx = provider->compile_expr(expr);
    if (ctx != nullptr) {
        // Ensure the provider back-pointer is set.
        if (ctx->provider == nullptr) {
            ctx->provider = provider;
        }
        g_stats.compile_successes++;
    }

    // Cache the result (including nullptr for "compilation failed").
    auto& cache = GetCache();
    cache[expr] = {ctx, true};

    return ctx;
}

Datum JitEvalExpr(JitContext* ctx, pgcpp::executor::ExprContext* econtext, bool* isNull) {
    if (ctx == nullptr || ctx->eval_func == nullptr) {
        *isNull = true;
        return 0;
    }
    return ctx->eval_func(ctx->private_state, econtext, isNull);
}

void JitReleaseContext(JitContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const JitProvider* provider = ctx->provider;
    if (provider != nullptr && provider->release_context != nullptr) {
        provider->release_context(ctx);
    }
}

// ---------------------------------------------------------------------------
// JIT context cache
// ---------------------------------------------------------------------------

JitContext* GetCachedJitContext(Node* expr) {
    auto& cache = GetCache();
    auto it = cache.find(expr);
    if (it == cache.end()) {
        g_stats.cache_misses++;
        return nullptr;  // Not in cache — not yet compiled.
    }
    if (it->second.ctx != nullptr) {
        g_stats.cache_hits++;
    }
    return it->second.ctx;
}

void ClearJitCache() {
    auto& cache = GetCache();
    // Release all cached contexts before clearing.
    for (auto& [node, entry] : cache) {
        if (entry.ctx != nullptr) {
            JitReleaseContext(entry.ctx);
        }
    }
    cache.clear();
}

// ---------------------------------------------------------------------------
// GUC accessors
// ---------------------------------------------------------------------------

bool IsJitEnabled() {
    return g_jit_enabled;
}

void SetJitEnabled(bool enabled) {
    g_jit_enabled = enabled;
}

int GetJitAboveCost() {
    return g_jit_above_cost;
}

void SetJitAboveCost(int cost) {
    g_jit_above_cost = cost;
}

int GetJitInlineAboveCost() {
    return g_jit_inline_above_cost;
}

void SetJitInlineAboveCost(int cost) {
    g_jit_inline_above_cost = cost;
}

int GetJitOptimizeAboveCost() {
    return g_jit_optimize_above_cost;
}

void SetJitOptimizeAboveCost(int cost) {
    g_jit_optimize_above_cost = cost;
}

const char* GetJitProviderName() {
    const JitProvider* provider = GetJitProvider();
    if (provider != nullptr && provider->provider_name != nullptr) {
        return provider->provider_name();
    }
    return "none";
}

JitStats GetJitStats() {
    return g_stats;
}

}  // namespace pgcpp::jit
