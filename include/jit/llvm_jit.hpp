// llvm_jit.h — LLVM-based JIT expression compilation provider.
//
// Converted from PostgreSQL 15's src/backend/jit/llvm/llvm.c.
//
// When LLVM is available (ENABLE_LLVM_JIT=ON at build time), this provider
// compiles simple SQL expressions (Var, Const, OpExpr with int4/float8
// arithmetic and comparison) to native code using LLVM's MCJIT engine.
//
// When LLVM is not available, IsLlvmJitAvailable() returns false and
// RegisterLlvmJitProvider() is a no-op that returns false.
#pragma once

namespace pgcpp::jit {

// IsLlvmJitAvailable — returns true if the LLVM JIT provider is compiled in
// (i.e., the build was configured with ENABLE_LLVM_JIT=ON and LLVM was found).
bool IsLlvmJitAvailable();

// RegisterLlvmJitProvider — register the LLVM JIT provider as the active
// JIT provider. Also enables JIT (calls SetJitEnabled(true)).
// Returns true on success, false if LLVM is not available.
bool RegisterLlvmJitProvider();

}  // namespace pgcpp::jit
