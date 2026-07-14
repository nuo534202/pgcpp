// llvm_jit.cpp — LLVM-based JIT expression compilation provider.
//
// Converted from PostgreSQL 15's src/backend/jit/llvm/llvm.c and
// src/backend/jit/llvm/llvm_expr.c.
//
// This file has two modes:
//   1. PGCPP_LLVM_JIT defined: full LLVM-based JIT compilation using the
//      LLVM C API (Core.h + ExecutionEngine.h) and MCJIT.
//   2. PGCPP_LLVM_JIT not defined: stub implementation that always returns
//      false/nullptr. This is the default (used in CI where LLVM is not
//      installed).
//
// LLVM JIT compilation strategy:
//   The compiled function receives an array of pre-evaluated Datum values
//   and null flags for leaf expressions (Var, Const). The function body
//   contains the arithmetic/comparison operations on those values.
//
//   Function signature (LLVM):
//     define i64 @jit_expr(i64* %values, i8* %isnull, i32 %nargs)
//
//   The wrapper (LlvmEvalWrapper) evaluates leaf nodes using the
//   interpreter, fills the arrays, calls the compiled function, and
//   returns the result Datum.
//
// Supported expressions:
//   - Var (leaf: pre-evaluated by interpreter)
//   - Const (leaf: pre-evaluated by interpreter)
//   - OpExpr (binary): +, -, *, / on int4/int8/float8
//                       =, <>, <, <=, >, >= on int4/int8/float8
#include "jit/llvm_jit.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_operator.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/exec_expr.hpp"
#include "executor/tupletable.hpp"
#include "jit/jit.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

namespace pgcpp::jit {

using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::executor::ExprContext;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::palloc;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Const;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Var;
using pgcpp::types::kBoolOid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;

#ifdef PGCPP_LLVM_JIT

// ---------------------------------------------------------------------------
// LLVM C API includes
// ---------------------------------------------------------------------------
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>

// ---------------------------------------------------------------------------
// LlvmJitState — private state for each compiled expression
// ---------------------------------------------------------------------------

struct LlvmJitState {
    LLVMContextRef llvm_context = nullptr;
    LLVMModuleRef llvm_module = nullptr;
    LLVMExecutionEngineRef engine = nullptr;

    // The compiled function pointer.
    // Signature: int64_t (int64_t* values, uint8_t* isnull, int32_t nargs)
    int64_t (*func)(int64_t*, uint8_t*, int32_t) = nullptr;

    // Leaf nodes to pre-evaluate (Var, Const) in evaluation order.
    std::vector<Node*> leaves;
};

// ---------------------------------------------------------------------------
// Expression analysis: collect leaves and verify compilability
// ---------------------------------------------------------------------------

namespace {

// Result of expression analysis.
struct ExprAnalysis {
    bool compilable = true;
    std::vector<Node*> leaves;
    // Operation type for the root OpExpr.
    Oid op_oid = 0;
    std::string op_name;
    Oid result_type = 0;
};

// Check if an operator is supported for JIT compilation.
bool IsSupportedOp(const std::string& opname, Oid lefttype) {
    if (lefttype != kInt4Oid && lefttype != kInt8Oid && lefttype != kFloat8Oid) {
        return false;
    }
    static const char* kSupportedOps[] = {"+", "-", "*", "/", "=", "<>", "<", "<=", ">", ">="};
    for (const char* op : kSupportedOps) {
        if (opname == op) {
            return true;
        }
    }
    return false;
}

// Analyze a single node. Returns true if the node (and all children) are
// compilable. Leaf nodes (Var, Const) are added to analysis.leaves.
bool AnalyzeNode(Node* node, ExprAnalysis* analysis) {
    if (node == nullptr) {
        analysis->compilable = false;
        return false;
    }

    NodeTag tag = node->GetTag();
    switch (tag) {
        case NodeTag::kVar:
        case NodeTag::kConst:
            // Leaf node — will be pre-evaluated by the interpreter.
            analysis->leaves.push_back(node);
            return true;

        case NodeTag::kOpExpr: {
            auto* op = static_cast<OpExpr*>(node);
            if (op->args.size() != 2) {
                analysis->compilable = false;
                return false;
            }
            // Look up operator info.
            const auto* oprow = GetCatalog()->GetOperatorByOid(op->opno);
            if (oprow == nullptr) {
                analysis->compilable = false;
                return false;
            }
            if (!IsSupportedOp(oprow->oprname, oprow->oprleft)) {
                analysis->compilable = false;
                return false;
            }
            // Recursively analyze arguments.
            if (!AnalyzeNode(op->args[0], analysis) || !AnalyzeNode(op->args[1], analysis)) {
                return false;
            }
            // Record root operation info.
            analysis->op_oid = op->opno;
            analysis->op_name = oprow->oprname;
            analysis->result_type = oprow->oprresult;
            return true;
        }

        default:
            // Unsupported node type.
            analysis->compilable = false;
            return false;
    }
}

// Get the leaf index for a node (position in analysis.leaves).
int GetLeafIndex(Node* node, const ExprAnalysis& analysis) {
    for (int i = 0; i < static_cast<int>(analysis.leaves.size()); i++) {
        if (analysis.leaves[i] == node) {
            return i;
        }
    }
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// LLVM IR generation
// ---------------------------------------------------------------------------

namespace {

// Generate LLVM IR for a node. Returns the LLVM value holding the result.
// For leaves (Var, Const), loads from the values array.
// For OpExpr, recursively generates IR for args and applies the operation.
LLVMValueRef GenIr(LLVMBuilderRef builder, LLVMValueRef values_ptr, LLVMValueRef isnull_ptr,
                   Node* node, const ExprAnalysis& analysis, LLVMContextRef ctx) {
    NodeTag tag = node->GetTag();

    if (tag == NodeTag::kVar || tag == NodeTag::kConst) {
        // Leaf: load from values array at leaf index.
        int idx = GetLeafIndex(node, analysis);
        // Get pointer: &values[idx]
        LLVMValueRef indices[] = {LLVMConstInt(LLVMInt32TypeInContext(ctx), idx, 0)};
        LLVMValueRef ptr =
            LLVMBuildGEP2(builder, LLVMInt64TypeInContext(ctx), values_ptr, indices, 1, "leaf_ptr");
        return LLVMBuildLoad2(builder, LLVMInt64TypeInContext(ctx), ptr, "leaf_val");
    }

    if (tag == NodeTag::kOpExpr) {
        auto* op = static_cast<OpExpr*>(node);
        // Recursively generate IR for left and right args.
        LLVMValueRef left = GenIr(builder, values_ptr, isnull_ptr, op->args[0], analysis, ctx);
        LLVMValueRef right = GenIr(builder, values_ptr, isnull_ptr, op->args[1], analysis, ctx);

        // Look up operator info.
        const auto* oprow = GetCatalog()->GetOperatorByOid(op->opno);
        Oid lefttype = oprow->oprleft;
        const std::string& opname = oprow->oprname;

        bool is_float = (lefttype == kFloat8Oid);
        bool is_int64 = (lefttype == kInt8Oid);

        if (is_float) {
            // For float8, Datum is 8 bytes (double stored via memcpy).
            // Treat the i64 as a double (bitcast).
            LLVMTypeRef double_type = LLVMDoubleTypeInContext(ctx);
            LLVMValueRef l = LLVMBuildBitCast(builder, left, double_type, "l_f");
            LLVMValueRef r = LLVMBuildBitCast(builder, right, double_type, "r_f");

            if (opname == "+")
                return LLVMBuildFAdd(builder, l, r, "add");
            if (opname == "-")
                return LLVMBuildFSub(builder, l, r, "sub");
            if (opname == "*")
                return LLVMBuildFMul(builder, l, r, "mul");
            if (opname == "/")
                return LLVMBuildFDiv(builder, l, r, "div");
            // Comparison: fcmp then zext to i64.
            LLVMRealPredicate pred = LLVMRealOEQ;
            if (opname == "=")
                pred = LLVMRealOEQ;
            else if (opname == "<>")
                pred = LLVMRealONE;
            else if (opname == "<")
                pred = LLVMRealOLT;
            else if (opname == "<=")
                pred = LLVMRealOLE;
            else if (opname == ">")
                pred = LLVMRealOGT;
            else if (opname == ">=")
                pred = LLVMRealOGE;
            LLVMValueRef cmp = LLVMBuildFCmp(builder, pred, l, r, "cmp");
            return LLVMBuildZExt(builder, cmp, LLVMInt64TypeInContext(ctx), "cmp_zext");
        }

        // Integer types: int4 (32-bit) or int8 (64-bit).
        // Datum is always 64-bit (uintptr_t). For int4, the value is
        // zero-extended into the 64-bit Datum. We operate on 32-bit or
        // 64-bit depending on the type.
        if (is_int64) {
            // 64-bit integer operations directly on i64.
            if (opname == "+")
                return LLVMBuildAdd(builder, left, right, "add");
            if (opname == "-")
                return LLVMBuildSub(builder, left, right, "sub");
            if (opname == "*")
                return LLVMBuildMul(builder, left, right, "mul");
            if (opname == "/")
                return LLVMBuildSDiv(builder, left, right, "div");
            // Comparison: icmp then zext.
            LLVMIntPredicate pred = LLVMIntEQ;
            if (opname == "=")
                pred = LLVMIntEQ;
            else if (opname == "<>")
                pred = LLVMIntNE;
            else if (opname == "<")
                pred = LLVMIntSLT;
            else if (opname == "<=")
                pred = LLVMIntSLE;
            else if (opname == ">")
                pred = LLVMIntSGT;
            else if (opname == ">=")
                pred = LLVMIntSGE;
            LLVMValueRef cmp = LLVMBuildICmp(builder, pred, left, right, "cmp");
            return LLVMBuildZExt(builder, cmp, LLVMInt64TypeInContext(ctx), "cmp_zext");
        }

        // int4: truncate to i32, operate, extend back to i64.
        LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
        LLVMValueRef l32 = LLVMBuildTrunc(builder, left, i32, "l32");
        LLVMValueRef r32 = LLVMBuildTrunc(builder, right, i32, "r32");

        if (opname == "+") {
            LLVMValueRef res = LLVMBuildAdd(builder, l32, r32, "add");
            return LLVMBuildZExt(builder, res, LLVMInt64TypeInContext(ctx), "add_ext");
        }
        if (opname == "-") {
            LLVMValueRef res = LLVMBuildSub(builder, l32, r32, "sub");
            return LLVMBuildZExt(builder, res, LLVMInt64TypeInContext(ctx), "sub_ext");
        }
        if (opname == "*") {
            LLVMValueRef res = LLVMBuildMul(builder, l32, r32, "mul");
            return LLVMBuildZExt(builder, res, LLVMInt64TypeInContext(ctx), "mul_ext");
        }
        if (opname == "/") {
            LLVMValueRef res = LLVMBuildSDiv(builder, l32, r32, "div");
            return LLVMBuildZExt(builder, res, LLVMInt64TypeInContext(ctx), "div_ext");
        }
        // Comparison on i32.
        LLVMIntPredicate pred = LLVMIntEQ;
        if (opname == "=")
            pred = LLVMIntEQ;
        else if (opname == "<>")
            pred = LLVMIntNE;
        else if (opname == "<")
            pred = LLVMIntSLT;
        else if (opname == "<=")
            pred = LLVMIntSLE;
        else if (opname == ">")
            pred = LLVMIntSGT;
        else if (opname == ">=")
            pred = LLVMIntSGE;
        LLVMValueRef cmp = LLVMBuildICmp(builder, pred, l32, r32, "cmp");
        return LLVMBuildZExt(builder, cmp, LLVMInt64TypeInContext(ctx), "cmp_zext");
    }

    return nullptr;  // Unsupported node type.
}

}  // namespace

// ---------------------------------------------------------------------------
// Evaluation wrapper
// ---------------------------------------------------------------------------

// LlvmEvalWrapper — the JitEvalFunc wrapper that pre-evaluates leaf nodes
// and calls the compiled function.
Datum LlvmEvalWrapper(void* state, ExprContext* econtext, bool* isNull) {
    auto* jstate = static_cast<LlvmJitState*>(state);
    if (jstate == nullptr || jstate->func == nullptr) {
        *isNull = true;
        return 0;
    }

    // Pre-evaluate leaf nodes using the interpreter.
    int nleaves = static_cast<int>(jstate->leaves.size());
    // Use stack-allocated arrays for small leaf counts, heap for larger.
    constexpr int kStackMax = 16;
    int64_t stack_values[kStackMax];
    uint8_t stack_isnull[kStackMax];
    int64_t* values = stack_values;
    uint8_t* isnull = stack_isnull;
    if (nleaves > kStackMax) {
        values = new int64_t[nleaves];
        isnull = new uint8_t[nleaves];
    }

    bool any_null = false;
    for (int i = 0; i < nleaves; i++) {
        bool leaf_null = false;
        Datum leaf_val = pgcpp::executor::ExecEvalExpr(jstate->leaves[i], econtext, &leaf_null);
        values[i] = static_cast<int64_t>(leaf_val);
        isnull[i] = leaf_null ? 1 : 0;
        if (leaf_null) {
            any_null = true;
        }
    }

    // SQL three-valued logic: if any operand is NULL, result is NULL.
    if (any_null) {
        *isNull = true;
        if (nleaves > kStackMax) {
            delete[] values;
            delete[] isnull;
        }
        return 0;
    }

    // Call the compiled function.
    int64_t result = jstate->func(values, isnull, nleaves);

    if (nleaves > kStackMax) {
        delete[] values;
        delete[] isnull;
    }

    *isNull = false;
    return static_cast<Datum>(result);
}

// ---------------------------------------------------------------------------
// Provider callbacks
// ---------------------------------------------------------------------------

JitContext* LlvmCompileExpr(Node* expr) {
    if (expr == nullptr) {
        return nullptr;
    }

    // Only compile OpExpr (the root must be an operation).
    if (expr->GetTag() != NodeTag::kOpExpr) {
        return nullptr;
    }

    // Analyze the expression tree.
    ExprAnalysis analysis;
    if (!AnalyzeNode(expr, &analysis) || !analysis.compilable) {
        return nullptr;
    }

    // Initialize LLVM (once).
    static bool llvm_initialized = false;
    if (!llvm_initialized) {
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
        llvm_initialized = true;
    }

    // Create LLVM context and module.
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("jit_expr", ctx);

    // Create function type: i64 (i64*, i8*, i32)
    LLVMTypeRef param_types[] = {
        LLVMPointerType(LLVMInt64TypeInContext(ctx), 0),  // values
        LLVMPointerType(LLVMInt8TypeInContext(ctx), 0),   // isnull
        LLVMInt32TypeInContext(ctx),                      // nargs
    };
    LLVMTypeRef func_type = LLVMFunctionType(LLVMInt64TypeInContext(ctx), param_types, 3, 0);
    LLVMValueRef func = LLVMAddFunction(module, "jit_expr", func_type);

    // Create entry basic block.
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, func, "entry");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
    LLVMPositionBuilderAtEnd(builder, entry);

    // Get parameters.
    LLVMValueRef values_ptr = LLVMGetParam(func, 0);
    LLVMValueRef isnull_ptr = LLVMGetParam(func, 1);
    (void)isnull_ptr;  // Not used in current implementation (null check in wrapper)
    LLVMValueRef nargs = LLVMGetParam(func, 2);
    (void)nargs;

    // Generate IR for the expression.
    LLVMValueRef result = GenIr(builder, values_ptr, isnull_ptr, expr, analysis, ctx);
    if (result == nullptr) {
        LLVMDisposeBuilder(builder);
        LLVMDisposeModule(module);
        LLVMContextDispose(ctx);
        return nullptr;
    }

    // Return the result.
    LLVMBuildRet(builder, result);

    // Verify the module.
    char* error = nullptr;
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &error)) {
        // Verification failed.
        if (error != nullptr) {
            LLVMDisposeMessage(error);
        }
        LLVMDisposeBuilder(builder);
        LLVMDisposeModule(module);
        LLVMContextDispose(ctx);
        return nullptr;
    }
    if (error != nullptr) {
        LLVMDisposeMessage(error);
    }

    // Create MCJIT execution engine.
    LLVMLinkInMCJIT();
    LLVMExecutionEngineRef engine = nullptr;
    if (LLVMCreateExecutionEngineForModule(&engine, module, &error)) {
        // Engine creation failed.
        if (error != nullptr) {
            LLVMDisposeMessage(error);
        }
        LLVMDisposeBuilder(builder);
        LLVMDisposeModule(module);
        LLVMContextDispose(ctx);
        return nullptr;
    }

    // Get the function address.
    uint64_t func_addr = LLVMGetFunctionAddress(engine, "jit_expr");
    if (func_addr == 0) {
        LLVMDisposeExecutionEngine(engine);
        LLVMDisposeBuilder(builder);
        LLVMContextDispose(ctx);
        return nullptr;
    }

    // Create the JIT state.
    auto* state = new LlvmJitState();
    state->llvm_context = ctx;
    state->llvm_module = module;  // Owned by the engine, but we keep the ref.
    state->engine = engine;
    state->func = reinterpret_cast<int64_t (*)(int64_t*, uint8_t*, int32_t)>(func_addr);
    state->leaves = std::move(analysis.leaves);

    LLVMDisposeBuilder(builder);

    // Create the JitContext.
    auto* jit_ctx = new JitContext();
    jit_ctx->eval_func = &LlvmEvalWrapper;
    jit_ctx->private_state = state;
    return jit_ctx;
}

void LlvmReleaseContext(JitContext* ctx) {
    if (ctx == nullptr || ctx->private_state == nullptr) {
        return;
    }
    auto* state = static_cast<LlvmJitState*>(ctx->private_state);
    if (state->engine != nullptr) {
        LLVMDisposeExecutionEngine(state->engine);
    }
    // Note: LLVMDisposeExecutionEngine also disposes the module.
    // Do not call LLVMDisposeModule separately.
    if (state->llvm_context != nullptr) {
        LLVMContextDispose(state->llvm_context);
    }
    delete state;
    delete ctx;
}

const char* LlvmProviderName() {
    return "llvm";
}

const JitProvider kLlvmProvider = {
    LlvmCompileExpr,
    LlvmReleaseContext,
    LlvmProviderName,
};

// ---------------------------------------------------------------------------
// Public API (LLVM available)
// ---------------------------------------------------------------------------

bool IsLlvmJitAvailable() {
    return true;
}

bool RegisterLlvmJitProvider() {
    SetJitProvider(&kLlvmProvider);
    SetJitEnabled(true);
    return true;
}

#else  // !PGCPP_LLVM_JIT

// ---------------------------------------------------------------------------
// Stub implementation (LLVM not available)
// ---------------------------------------------------------------------------

bool IsLlvmJitAvailable() {
    return false;
}

bool RegisterLlvmJitProvider() {
    return false;
}

#endif  // PGCPP_LLVM_JIT

}  // namespace pgcpp::jit
