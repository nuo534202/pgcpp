// jit_test.cpp — Unit tests for the JIT subsystem (P3-6).
//
// Tests three layers:
//   1. JIT framework: GUC variables, provider management, compilation cache,
//      statistics, and the default interpreter provider (no-op).
//   2. A custom pluggable provider: verifies the provider interface contract
//      (compile, eval, cache, release, stats).
//   3. Executor integration: ExecQual / ExecProject with JIT enabled and
//      disabled, verifying interpreter fallback when the provider returns
//      nullptr.
//
// The LLVM provider is conditionally compiled (PGCPP_LLVM_JIT). In default
// builds (no LLVM), IsLlvmJitAvailable() returns false and the LLVM-specific
// tests are skipped.

#include "jit/jit.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_operator.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/exec_expr.hpp"
#include "executor/tupletable.hpp"
#include "jit/llvm_jit.hpp"
#include "parser/primnodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::InitializeRelcache;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::executor::CreateExprContext;
using pgcpp::executor::ExecProject;
using pgcpp::executor::ExecQual;
using pgcpp::executor::ExprContext;
using pgcpp::executor::MakeTupleTableSlot;
using pgcpp::executor::ResetExprContext;
using pgcpp::executor::TupleTableSlot;
using pgcpp::jit::ClearJitCache;
using pgcpp::jit::GetCachedJitContext;
using pgcpp::jit::GetInterpreterProvider;
using pgcpp::jit::GetJitAboveCost;
using pgcpp::jit::GetJitInlineAboveCost;
using pgcpp::jit::GetJitOptimizeAboveCost;
using pgcpp::jit::GetJitProvider;
using pgcpp::jit::GetJitProviderName;
using pgcpp::jit::GetJitStats;
using pgcpp::jit::InitJit;
using pgcpp::jit::IsJitEnabled;
using pgcpp::jit::IsLlvmJitAvailable;
using pgcpp::jit::JitCompileExpr;
using pgcpp::jit::JitContext;
using pgcpp::jit::JitEvalExpr;
using pgcpp::jit::JitProvider;
using pgcpp::jit::JitReleaseContext;
using pgcpp::jit::JitStats;
using pgcpp::jit::RegisterLlvmJitProvider;
using pgcpp::jit::SetJitAboveCost;
using pgcpp::jit::SetJitEnabled;
using pgcpp::jit::SetJitInlineAboveCost;
using pgcpp::jit::SetJitOptimizeAboveCost;
using pgcpp::jit::SetJitProvider;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::Const;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;      // int4 = int4
constexpr Oid kInt4NeOp = 80;      // int4 <> int4
constexpr Oid kInt4LtOp = 97;      // int4 < int4
constexpr Oid kInt4LeOp = 523;     // int4 <= int4
constexpr Oid kInt4GtOp = 521;     // int4 > int4
constexpr Oid kInt4GeOp = 525;     // int4 >= int4
constexpr Oid kInt4PlusOp = 551;   // int4 + int4
constexpr Oid kInt4MinusOp = 550;  // int4 - int4
constexpr Oid kInt4MulOp = 552;    // int4 * int4
constexpr Oid kInt4DivOp = 553;    // int4 / int4

// ---------------------------------------------------------------------------
// Custom test provider — a minimal provider that always "compiles" an
// expression to a function returning a fixed Datum (42). Used to verify the
// pluggable provider interface without requiring LLVM.
// ---------------------------------------------------------------------------

pgcpp::jit::Datum CustomEval(void* /*state*/, ExprContext* /*econtext*/, bool* isNull) {
    *isNull = false;
    return static_cast<pgcpp::jit::Datum>(42);
}

JitContext* CustomCompile(Node* /*expr*/) {
    auto* ctx = new JitContext();
    ctx->eval_func = &CustomEval;
    ctx->private_state = nullptr;
    ctx->provider = nullptr;  // Set by JitCompileExpr.
    return ctx;
}

void CustomRelease(JitContext* ctx) {
    delete ctx;
}

const char* CustomName() {
    return "custom";
}

const JitProvider kCustomProvider = {CustomCompile, CustomRelease, CustomName};

}  // namespace

// ===========================================================================
// Part 1: JIT framework tests (lightweight fixture — memory context only)
// ===========================================================================

class JitFrameworkTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("jit_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
        // Release any leftover cached contexts from a previous test, then
        // reset all JIT state to defaults.
        ClearJitCache();
        InitJit();
    }

    void TearDown() override {
        ClearJitCache();
        InitJit();  // Reset to defaults for test isolation.
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: create a Const node for use as a cache key.
    Node* MakeExpr() {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(1);
        con->constisnull = false;
        return con;
    }

    AllocSetContext* context_ = nullptr;
};

// --- InitJit / defaults ---

TEST_F(JitFrameworkTest, InitJit_ResetsDefaults) {
    // After InitJit, JIT should be disabled with default cost thresholds.
    EXPECT_FALSE(IsJitEnabled());
    EXPECT_EQ(GetJitAboveCost(), 100000);
    EXPECT_EQ(GetJitInlineAboveCost(), 500000);
    EXPECT_EQ(GetJitOptimizeAboveCost(), 500000);
}

TEST_F(JitFrameworkTest, InitJit_ResetsProvider) {
    // After InitJit, the active provider should be the interpreter.
    EXPECT_EQ(GetJitProvider(), GetInterpreterProvider());
}

TEST_F(JitFrameworkTest, InitJit_ResetsStats) {
    // Do some operations to populate stats.
    SetJitEnabled(true);
    auto* expr = MakeExpr();
    (void)JitCompileExpr(expr);
    (void)GetCachedJitContext(expr);

    // InitJit should zero the stats.
    InitJit();
    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.compile_attempts, 0);
    EXPECT_EQ(stats.compile_successes, 0);
    EXPECT_EQ(stats.cache_hits, 0);
    EXPECT_EQ(stats.cache_misses, 0);
}

// --- GUC accessors ---

TEST_F(JitFrameworkTest, Guc_JitEnabled) {
    EXPECT_FALSE(IsJitEnabled());
    SetJitEnabled(true);
    EXPECT_TRUE(IsJitEnabled());
    SetJitEnabled(false);
    EXPECT_FALSE(IsJitEnabled());
}

TEST_F(JitFrameworkTest, Guc_JitAboveCost) {
    SetJitAboveCost(50000);
    EXPECT_EQ(GetJitAboveCost(), 50000);
    SetJitAboveCost(0);
    EXPECT_EQ(GetJitAboveCost(), 0);
}

TEST_F(JitFrameworkTest, Guc_JitInlineAboveCost) {
    SetJitInlineAboveCost(250000);
    EXPECT_EQ(GetJitInlineAboveCost(), 250000);
}

TEST_F(JitFrameworkTest, Guc_JitOptimizeAboveCost) {
    SetJitOptimizeAboveCost(300000);
    EXPECT_EQ(GetJitOptimizeAboveCost(), 300000);
}

// --- Provider management ---

TEST_F(JitFrameworkTest, Provider_GetInterpreter) {
    const JitProvider* p = GetInterpreterProvider();
    ASSERT_NE(p, nullptr);
    ASSERT_NE(p->compile_expr, nullptr);
    ASSERT_NE(p->release_context, nullptr);
    ASSERT_NE(p->provider_name, nullptr);
    EXPECT_STREQ(p->provider_name(), "interpreter");
}

TEST_F(JitFrameworkTest, Provider_DefaultIsInterpreter) {
    EXPECT_EQ(GetJitProvider(), GetInterpreterProvider());
}

TEST_F(JitFrameworkTest, Provider_SetNull_RevertsToInterpreter) {
    SetJitProvider(&kCustomProvider);
    EXPECT_EQ(GetJitProvider(), &kCustomProvider);
    SetJitProvider(nullptr);
    EXPECT_EQ(GetJitProvider(), GetInterpreterProvider());
}

TEST_F(JitFrameworkTest, Provider_Name) {
    EXPECT_STREQ(GetJitProviderName(), "interpreter");
    SetJitProvider(&kCustomProvider);
    EXPECT_STREQ(GetJitProviderName(), "custom");
}

// --- Compilation ---

TEST_F(JitFrameworkTest, Compile_Disabled_ReturnsNull) {
    // JIT disabled by default.
    auto* expr = MakeExpr();
    EXPECT_EQ(JitCompileExpr(expr), nullptr);
}

TEST_F(JitFrameworkTest, Compile_Interpreter_ReturnsNull) {
    SetJitEnabled(true);
    auto* expr = MakeExpr();
    // Interpreter provider never compiles.
    EXPECT_EQ(JitCompileExpr(expr), nullptr);
}

TEST_F(JitFrameworkTest, Compile_NullExpr_NoCrash) {
    SetJitEnabled(true);
    EXPECT_EQ(JitCompileExpr(nullptr), nullptr);
}

TEST_F(JitFrameworkTest, Compile_IncrementsAttempts) {
    SetJitEnabled(true);
    auto* expr = MakeExpr();
    (void)JitCompileExpr(expr);
    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.compile_attempts, 1);
    // Interpreter provider returns nullptr → no success.
    EXPECT_EQ(stats.compile_successes, 0);
}

// --- Cache ---

TEST_F(JitFrameworkTest, Cache_UnseenExpr_ReturnsNull) {
    auto* expr = MakeExpr();
    EXPECT_EQ(GetCachedJitContext(expr), nullptr);
    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.cache_misses, 1);
    EXPECT_EQ(stats.cache_hits, 0);
}

TEST_F(JitFrameworkTest, Cache_AfterFailedCompile) {
    SetJitEnabled(true);
    auto* expr = MakeExpr();
    // Compilation with interpreter fails (returns nullptr), but the result
    // is cached so we don't retry.
    EXPECT_EQ(JitCompileExpr(expr), nullptr);
    // Second lookup: the cache has an entry (ctx=nullptr, compiled=true).
    // GetCachedJitContext returns nullptr (cached failure).
    EXPECT_EQ(GetCachedJitContext(expr), nullptr);
    // The second lookup should NOT increment cache_misses (it's in the cache).
    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.cache_misses, 0);  // Only GetCachedJitContext calls count.
    // compile_attempts should still be 1 (not retried).
    EXPECT_EQ(stats.compile_attempts, 1);
}

TEST_F(JitFrameworkTest, ClearCache_RemovesEntries) {
    SetJitEnabled(true);
    auto* expr = MakeExpr();
    (void)JitCompileExpr(expr);
    ClearJitCache();
    // After clearing, the expression is no longer cached.
    auto* ctx = GetCachedJitContext(expr);
    EXPECT_EQ(ctx, nullptr);
    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.cache_misses, 1);  // New miss after clear.
}

// --- Eval ---

TEST_F(JitFrameworkTest, Eval_NullContext) {
    bool isNull = false;
    Datum result = JitEvalExpr(nullptr, nullptr, &isNull);
    EXPECT_TRUE(isNull);
    EXPECT_EQ(result, Datum(0));
}

TEST_F(JitFrameworkTest, Eval_NullEvalFunc) {
    JitContext ctx;
    ctx.eval_func = nullptr;
    bool isNull = false;
    Datum result = JitEvalExpr(&ctx, nullptr, &isNull);
    EXPECT_TRUE(isNull);
    EXPECT_EQ(result, Datum(0));
}

// --- Release ---

TEST_F(JitFrameworkTest, Release_NullContext) {
    // Should not crash.
    JitReleaseContext(nullptr);
}

// --- Custom provider ---

TEST_F(JitFrameworkTest, CustomProvider_CompileAndEval) {
    SetJitProvider(&kCustomProvider);
    SetJitEnabled(true);
    auto* expr = MakeExpr();
    JitContext* ctx = JitCompileExpr(expr);
    ASSERT_NE(ctx, nullptr);
    ASSERT_NE(ctx->eval_func, nullptr);
    ASSERT_EQ(ctx->provider, &kCustomProvider);

    bool isNull = true;
    Datum result = JitEvalExpr(ctx, nullptr, &isNull);
    EXPECT_FALSE(isNull);
    EXPECT_EQ(result, Datum(42));

    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.compile_attempts, 1);
    EXPECT_EQ(stats.compile_successes, 1);
}

TEST_F(JitFrameworkTest, CustomProvider_CacheHit) {
    SetJitProvider(&kCustomProvider);
    SetJitEnabled(true);
    auto* expr = MakeExpr();
    JitContext* ctx1 = JitCompileExpr(expr);
    ASSERT_NE(ctx1, nullptr);

    // Second call: should be served from cache (no new compilation).
    JitContext* ctx2 = GetCachedJitContext(expr);
    EXPECT_EQ(ctx2, ctx1);

    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.compile_attempts, 1);
    EXPECT_EQ(stats.cache_hits, 1);
    EXPECT_EQ(stats.cache_misses, 0);
}

TEST_F(JitFrameworkTest, CustomProvider_ReleasedOnClear) {
    SetJitProvider(&kCustomProvider);
    SetJitEnabled(true);
    auto* expr = MakeExpr();
    JitContext* ctx = JitCompileExpr(expr);
    ASSERT_NE(ctx, nullptr);

    // ClearJitCache should call CustomRelease (which deletes ctx) and empty
    // the cache. After clearing, GetCachedJitContext should miss.
    ClearJitCache();
    JitContext* cached = GetCachedJitContext(expr);
    EXPECT_EQ(cached, nullptr);  // Cache was cleared.
    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.cache_misses, 1);  // Miss after clear.

    // Recompiling should create a new context and work correctly.
    JitContext* ctx2 = JitCompileExpr(expr);
    ASSERT_NE(ctx2, nullptr);
    bool isNull = true;
    Datum result = JitEvalExpr(ctx2, nullptr, &isNull);
    EXPECT_FALSE(isNull);
    EXPECT_EQ(result, Datum(42));
}

TEST_F(JitFrameworkTest, CustomProvider_StatsOnSuccess) {
    SetJitProvider(&kCustomProvider);
    SetJitEnabled(true);
    auto* expr1 = MakeExpr();
    auto* expr2 = MakeExpr();
    (void)JitCompileExpr(expr1);
    (void)JitCompileExpr(expr2);

    JitStats stats = GetJitStats();
    EXPECT_EQ(stats.compile_attempts, 2);
    EXPECT_EQ(stats.compile_successes, 2);
}

// --- LLVM stub (default build without LLVM) ---

TEST_F(JitFrameworkTest, Llvm_NotAvailable) {
    // In default builds (no PGCPP_LLVM_JIT), LLVM is not available.
    if (!IsLlvmJitAvailable()) {
        GTEST_SKIP() << "LLVM JIT not compiled in";
    }
}

TEST_F(JitFrameworkTest, Llvm_RegisterFails_WhenNotAvailable) {
    if (IsLlvmJitAvailable()) {
        GTEST_SKIP() << "LLVM JIT is compiled in — skipping stub test";
    }
    EXPECT_FALSE(RegisterLlvmJitProvider());
}

// ===========================================================================
// Part 2: Executor integration tests (full stack fixture)
//
// These tests verify that ExecQual and ExecProject correctly use the JIT
// fast path when enabled and fall back to the interpreter when the provider
// returns nullptr.
// ===========================================================================

class JitExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("jit_exec_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        pgcpp::transaction::InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_jit_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();

        // Reset JIT state.
        ClearJitCache();
        InitJit();
    }

    void TearDown() override {
        ClearJitCache();
        InitJit();

        EndTransactionBlock();
        ResetRelcache();
        ShutdownBufferPool();
        smgrcloseall();
        RunShell("rm -rf " + test_dir_);

        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        ResetTransactionState();
        InitializeTransactionSystem();
        pgcpp::transaction::InitializeSnapshotManager();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: build a pg_class row.
    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kRelation;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

    // Helper: build a 2-column int4 schema (a, b).
    std::vector<FormData_pg_attribute> MakeIntIntSchema(Oid relid) {
        FormData_pg_attribute a1;
        a1.attrelid = relid;
        a1.attname = "a";
        a1.attnum = 1;
        a1.atttypid = kInt4Oid;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;

        FormData_pg_attribute a2;
        a2.attrelid = relid;
        a2.attname = "b";
        a2.attnum = 2;
        a2.atttypid = kInt4Oid;
        a2.attlen = 4;
        a2.attbyval = true;
        a2.attalign = AttAlign::kInt;
        a2.attstorage = AttStorage::kPlain;

        return {a1, a2};
    }

    // Helper: create a relation with the given OID and schema.
    void CreateTestRelation(Oid relid, const std::string& name,
                            const std::vector<FormData_pg_attribute>& attrs) {
        catalog_->InsertClass(MakeClassRow(name, relid));
        for (const auto& attr : attrs) {
            auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
            catalog_->InsertAttribute(attr_row);
        }
    }

    // Helper: create a Var node.
    Var* MakeVar(int varno, int varattno, Oid vartype) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = vartype;
        return var;
    }

    // Helper: create a Const node for int4.
    Const* MakeInt4Const(int32_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    // Helper: create an OpExpr.
    OpExpr* MakeOpExpr(Oid opno, Oid resulttype, Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = opno;
        op->opresulttype = resulttype;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    // Helper: create a TargetEntry.
    TargetEntry* MakeTargetEntry(Node* expr, int resno) {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = "";
        return te;
    }

    // Helper: set up a scan tuple slot with (a, b) values.
    TupleTableSlot* MakeSlot(Oid relid, int32_t a, int32_t b) {
        auto attrs = MakeIntIntSchema(relid);
        CreateTestRelation(relid, "test_rel", attrs);
        auto rel = RelationOpen(relid);
        TupleTableSlot* slot = MakeTupleTableSlot(rel->rd_att);
        Datum values[2] = {Int32GetDatum(a), Int32GetDatum(b)};
        bool isnull[2] = {false, false};
        slot->StoreVirtual(values, isnull);
        RelationClose(rel);
        return slot;
    }

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// --- ExecQual with JIT disabled (interpreter path) ---

TEST_F(JitExecutorTest, ExecQual_JitDisabled_True) {
    Oid relid = kFirstNormalObjectId;
    TupleTableSlot* slot = MakeSlot(relid, 10, 20);
    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // a = 10 → true
    Var* a = MakeVar(1, 1, kInt4Oid);
    Const* c = MakeInt4Const(10);
    OpExpr* eq = MakeOpExpr(kInt4EqOp, kBoolOid, a, c);

    EXPECT_FALSE(IsJitEnabled());
    EXPECT_TRUE(ExecQual(eq, econtext));

    ResetExprContext(econtext);
}

TEST_F(JitExecutorTest, ExecQual_JitDisabled_False) {
    Oid relid = kFirstNormalObjectId;
    TupleTableSlot* slot = MakeSlot(relid, 10, 20);
    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // a = 99 → false
    Var* a = MakeVar(1, 1, kInt4Oid);
    Const* c = MakeInt4Const(99);
    OpExpr* eq = MakeOpExpr(kInt4EqOp, kBoolOid, a, c);

    EXPECT_FALSE(ExecQual(eq, econtext));

    ResetExprContext(econtext);
}

// --- ExecQual with JIT enabled + interpreter provider (fallback) ---

TEST_F(JitExecutorTest, ExecQual_JitEnabled_InterpreterFallback_True) {
    SetJitEnabled(true);
    ASSERT_TRUE(IsJitEnabled());

    Oid relid = kFirstNormalObjectId;
    TupleTableSlot* slot = MakeSlot(relid, 10, 20);
    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // a < b → true (10 < 20)
    Var* a = MakeVar(1, 1, kInt4Oid);
    Var* b = MakeVar(1, 2, kInt4Oid);
    OpExpr* lt = MakeOpExpr(kInt4LtOp, kBoolOid, a, b);

    // JIT is enabled but interpreter provider returns nullptr → fallback.
    EXPECT_TRUE(ExecQual(lt, econtext));

    ResetExprContext(econtext);
}

TEST_F(JitExecutorTest, ExecQual_JitEnabled_InterpreterFallback_False) {
    SetJitEnabled(true);

    Oid relid = kFirstNormalObjectId;
    TupleTableSlot* slot = MakeSlot(relid, 50, 20);
    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // a < b → false (50 < 20 is false)
    Var* a = MakeVar(1, 1, kInt4Oid);
    Var* b = MakeVar(1, 2, kInt4Oid);
    OpExpr* lt = MakeOpExpr(kInt4LtOp, kBoolOid, a, b);

    EXPECT_FALSE(ExecQual(lt, econtext));

    ResetExprContext(econtext);
}

TEST_F(JitExecutorTest, ExecQual_JitEnabled_CompileAttempted) {
    SetJitEnabled(true);

    Oid relid = kFirstNormalObjectId;
    TupleTableSlot* slot = MakeSlot(relid, 10, 20);
    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    Var* a = MakeVar(1, 1, kInt4Oid);
    Const* c = MakeInt4Const(10);
    OpExpr* eq = MakeOpExpr(kInt4EqOp, kBoolOid, a, c);

    (void)ExecQual(eq, econtext);

    // JIT compilation should have been attempted (and failed with interpreter).
    JitStats stats = GetJitStats();
    EXPECT_GE(stats.compile_attempts, 1);

    ResetExprContext(econtext);
}

// --- ExecQual with null qual ---

TEST_F(JitExecutorTest, ExecQual_NullQual_Passes) {
    SetJitEnabled(true);
    ExprContext* econtext = CreateExprContext();
    // Null qual = all tuples pass.
    EXPECT_TRUE(ExecQual(nullptr, econtext));
    ResetExprContext(econtext);
}

// --- ExecProject with JIT disabled ---

TEST_F(JitExecutorTest, ExecProject_JitDisabled) {
    Oid relid = kFirstNormalObjectId;
    TupleTableSlot* slot = MakeSlot(relid, 10, 20);
    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // Project: a + b, a - b
    Var* a = MakeVar(1, 1, kInt4Oid);
    Var* b = MakeVar(1, 2, kInt4Oid);
    OpExpr* plus = MakeOpExpr(kInt4PlusOp, kInt4Oid, a, b);
    OpExpr* minus = MakeOpExpr(kInt4MinusOp, kInt4Oid, a, b);

    auto attrs = MakeIntIntSchema(relid);
    auto rel = RelationOpen(relid);
    TupleTableSlot* result = MakeTupleTableSlot(rel->rd_att);

    std::vector<TargetEntry*> tlist;
    tlist.push_back(MakeTargetEntry(plus, 1));
    tlist.push_back(MakeTargetEntry(minus, 2));

    ExecProject(tlist, econtext, result);

    EXPECT_EQ(DatumGetInt32(result->tts_values[0]), 30);   // 10 + 20
    EXPECT_EQ(DatumGetInt32(result->tts_values[1]), -10);  // 10 - 20

    RelationClose(rel);
    ResetExprContext(econtext);
}

// --- ExecProject with JIT enabled + interpreter (fallback) ---

TEST_F(JitExecutorTest, ExecProject_JitEnabled_InterpreterFallback) {
    SetJitEnabled(true);

    Oid relid = kFirstNormalObjectId;
    TupleTableSlot* slot = MakeSlot(relid, 10, 20);
    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    // Project: a * b, a > b
    Var* a = MakeVar(1, 1, kInt4Oid);
    Var* b = MakeVar(1, 2, kInt4Oid);
    OpExpr* mul = MakeOpExpr(kInt4MulOp, kInt4Oid, a, b);
    OpExpr* gt = MakeOpExpr(kInt4GtOp, kBoolOid, a, b);

    auto attrs = MakeIntIntSchema(relid);
    auto rel = RelationOpen(relid);
    TupleTableSlot* result = MakeTupleTableSlot(rel->rd_att);

    std::vector<TargetEntry*> tlist;
    tlist.push_back(MakeTargetEntry(mul, 1));
    tlist.push_back(MakeTargetEntry(gt, 2));

    ExecProject(tlist, econtext, result);

    EXPECT_EQ(DatumGetInt32(result->tts_values[0]), 200);       // 10 * 20
    EXPECT_TRUE(DatumGetBool(result->tts_values[1]) == false);  // 10 > 20 = false

    RelationClose(rel);
    ResetExprContext(econtext);
}

// --- ExecProject with Const-only target (no Var) ---

TEST_F(JitExecutorTest, ExecProject_ConstTarget_JitEnabled) {
    SetJitEnabled(true);

    Oid relid = kFirstNormalObjectId;
    TupleTableSlot* slot = MakeSlot(relid, 10, 20);
    ExprContext* econtext = CreateExprContext();
    econtext->ecxt_scantuple = slot;

    Const* c = MakeInt4Const(42);

    auto attrs = MakeIntIntSchema(relid);
    auto rel = RelationOpen(relid);
    TupleTableSlot* result = MakeTupleTableSlot(rel->rd_att);

    std::vector<TargetEntry*> tlist;
    tlist.push_back(MakeTargetEntry(c, 1));

    ExecProject(tlist, econtext, result);

    EXPECT_EQ(DatumGetInt32(result->tts_values[0]), 42);

    RelationClose(rel);
    ResetExprContext(econtext);
}
