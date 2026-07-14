// jit.h — JIT expression compilation provider framework.
//
// Converted from PostgreSQL 15's src/backend/jit/jit.c and
// src/include/jit/jit.h.
//
// PostgreSQL's JIT subsystem allows expression evaluation to be compiled
// to native code (typically via LLVM) instead of interpreted. The JIT
// provider is a pluggable interface: a default "interpreter" provider
// performs no compilation (expressions are always interpreted), while an
// LLVM-based provider (when available) compiles expressions to native code.
//
// Architecture:
//   - JitProvider: a struct of function pointers (compile, release, name).
//   - JitContext: holds a compiled function pointer and private state.
//   - GUC variables: jit, jit_above_cost, jit_inline_above_cost,
//     jit_optimize_above_cost.
//   - A per-expression cache (Node* -> JitContext*) for lazy compilation.
//
// Integration with the executor:
//   ExecEvalExpr() checks the JIT cache before dispatching to the
//   interpreter. If a JitContext exists for the expression, it calls
//   the compiled function; otherwise it falls through to the interpreter.
#pragma once

#include <cstdint>

namespace pgcpp::nodes {
class Node;
}  // namespace pgcpp::nodes

namespace pgcpp::executor {
struct ExprContext;
}  // namespace pgcpp::executor

namespace pgcpp::jit {

// Datum type alias (matches types/datum.hpp).
using Datum = uintptr_t;

// JitEvalFunc — signature of a JIT-compiled expression evaluation function.
//   state:     the JitContext's private_state (provider-specific).
//   econtext:  the executor's expression evaluation context.
//   isNull:    output parameter — set to true if the result is NULL.
// Returns the resulting Datum.
using JitEvalFunc = Datum (*)(void* state, pgcpp::executor::ExprContext* econtext, bool* isNull);

// JitContext — holds a compiled expression's function pointer and state.
// Returned by JitProvider::compile_expr. Freed by JitProvider::release_context.
struct JitContext {
    // The compiled evaluation function, or nullptr if compilation failed.
    JitEvalFunc eval_func = nullptr;

    // Provider-specific private state (e.g., LLVM execution engine handle).
    void* private_state = nullptr;

    // The provider that created this context (for release_context dispatch).
    const struct JitProvider* provider = nullptr;

    JitContext() = default;
    JitContext(JitEvalFunc f, void* s, const JitProvider* p)
        : eval_func(f), private_state(s), provider(p) {}
};

// JitProvider — pluggable JIT provider interface.
// A provider implements expression compilation to native code.
struct JitProvider {
    // Compile an expression to native code.
    // Returns a JitContext* (with eval_func set) on success, or nullptr
    // if the expression cannot be compiled (caller falls back to interpreter).
    JitContext* (*compile_expr)(pgcpp::nodes::Node* expr);

    // Release a previously compiled JitContext.
    void (*release_context)(JitContext* ctx);

    // Provider name (e.g., "interpreter", "llvm").
    const char* (*provider_name)();
};

// --- Initialization and provider management ---

// InitJit — initialize the JIT subsystem with the default interpreter
// provider. Safe to call multiple times; resets all state.
void InitJit();

// SetJitProvider — set the active JIT provider.
// Pass nullptr to revert to the interpreter provider.
void SetJitProvider(const JitProvider* provider);

// GetJitProvider — return the currently active provider.
const JitProvider* GetJitProvider();

// GetInterpreterProvider — return the built-in interpreter provider
// (compile_expr always returns nullptr).
const JitProvider* GetInterpreterProvider();

// --- Compilation and evaluation ---

// JitCompileExpr — attempt to compile an expression via the active provider.
// Returns a JitContext* on success, nullptr if compilation is not possible
// or JIT is disabled. The result is cached for subsequent calls with the
// same expression node.
JitContext* JitCompileExpr(pgcpp::nodes::Node* expr);

// JitEvalExpr — evaluate an expression using a previously compiled JitContext.
// This is a thin wrapper around ctx->eval_func.
Datum JitEvalExpr(JitContext* ctx, pgcpp::executor::ExprContext* econtext, bool* isNull);

// JitReleaseContext — release a compiled JitContext.
void JitReleaseContext(JitContext* ctx);

// --- JIT context cache ---

// GetCachedJitContext — look up the JIT cache for a previously compiled
// expression. Returns nullptr if not cached or if compilation was attempted
// and failed (cached as nullptr internally). This does NOT trigger
// compilation — use JitCompileExpr for that.
JitContext* GetCachedJitContext(pgcpp::nodes::Node* expr);

// ClearJitCache — clear all cached JIT contexts. Called between queries
// to avoid stale pointers.
void ClearJitCache();

// --- GUC accessors ---

// IsJitEnabled — returns true if JIT compilation is enabled (GUC: jit).
bool IsJitEnabled();

// SetJitEnabled — enable or disable JIT compilation.
void SetJitEnabled(bool enabled);

// GetJitAboveCost — minimum expression cost to trigger JIT compilation
// (GUC: jit_above_cost). 0 = always compile when enabled.
int GetJitAboveCost();

// SetJitAboveCost — set the JIT cost threshold.
void SetJitAboveCost(int cost);

// GetJitInlineAboveCost — cost threshold for inlining functions.
int GetJitInlineAboveCost();

// SetJitInlineAboveCost — set the inlining cost threshold.
void SetJitInlineAboveCost(int cost);

// GetJitOptimizeAboveCost — cost threshold for running LLVM optimizations.
int GetJitOptimizeAboveCost();

// SetJitOptimizeAboveCost — set the optimization cost threshold.
void SetJitOptimizeAboveCost(int cost);

// GetJitProviderName — name of the active provider (for EXPLAIN output).
const char* GetJitProviderName();

// GetJitStats — return statistics about JIT compilation.
struct JitStats {
    int compile_attempts = 0;   // total JitCompileExpr calls
    int compile_successes = 0;  // successful compilations
    int cache_hits = 0;         // cache lookups that found a compiled context
    int cache_misses = 0;       // cache lookups that found nothing
};

JitStats GetJitStats();

}  // namespace pgcpp::jit
