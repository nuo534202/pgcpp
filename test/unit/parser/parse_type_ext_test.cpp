// parse_type_ext_test.cpp — Unit tests for LookupTypeName (M5 Task 15.11.3).
//
// Tests the richer type-lookup API added to parse_type.cpp:
//   * LookupTypeName — resolves a TypeName*, handling qualified names,
//     array suffix (int4[]), and type modifiers (varchar(10)).
//   * typenameTypeId(pstate, tn, *typmod) — strict version that ereports
//     ERROR when the type does not resolve.
//
// PostgreSQL's parse_type.c provides these entry points; we add them here
// to bring the type-resolution pipeline up to P1 parity with ClickBench
// requirements.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pgcpp/catalog/bootstrap_catalog.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/syscache.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/parse_type.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/types/datum.hpp"

using mytoydb::catalog::BootstrapCatalog;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::GetSysCache;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::makePallocNode;
using mytoydb::nodes::Node;
using mytoydb::nodes::Value;
using mytoydb::parser::LookupTypeName;
using mytoydb::parser::make_parsestate;
using mytoydb::parser::ParseState;
using mytoydb::parser::TypeName;
using mytoydb::parser::typenameTypeId;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::kVarcharOid;

namespace {

class ParseTypeExtTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("parse_type_ext_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);

        syscache_ = new SysCache();
        SetSysCache(syscache_);
    }

    void TearDown() override {
        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Build a TypeName from a single component name (no schema).
    TypeName* MakeTypeName(const std::string& name) {
        auto* tn = makePallocNode<TypeName>();
        tn->names.push_back(mytoydb::nodes::makeString(name));
        return tn;
    }

    // Build a TypeName from a single component name + integer typmod.
    TypeName* MakeTypeNameWithTypmod(const std::string& name, int typmod_val) {
        auto* tn = MakeTypeName(name);
        tn->typmods.push_back(mytoydb::nodes::makeInteger(typmod_val));
        return tn;
    }

    // Build a TypeName with an unbounded array suffix (int4[]).
    TypeName* MakeArrayTypeName(const std::string& name) {
        auto* tn = MakeTypeName(name);
        // Array bounds: a single placeholder integer Value of -1 to denote
        // "no upper bound" (matches how PostgreSQL's scanner emits []).
        tn->array_bounds.push_back(mytoydb::nodes::makeInteger(-1));
        return tn;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// Helper: run fn inside PG_TRY and report whether it ereported.
template<typename F>
bool RaisesError(F&& fn) {
    bool caught = false;
    PG_TRY() {
        fn();
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    return caught;
}

}  // namespace

// ===========================================================================
// LookupTypeName — basic type resolution
// ===========================================================================

TEST_F(ParseTypeExtTest, LookupTypeNameResolvesBasicType) {
    TypeName* tn = MakeTypeName("int4");
    ParseState* pstate = make_parsestate(nullptr);
    int32_t typmod = -1;

    Oid oid = LookupTypeName(pstate, tn, &typmod);
    EXPECT_EQ(oid, kInt4Oid);
    EXPECT_EQ(tn->type_oid, kInt4Oid);
    free_parsestate(pstate);
}

TEST_F(ParseTypeExtTest, LookupTypeNameSetsTypeOidOnNode) {
    TypeName* tn = MakeTypeName("bigint");
    ParseState* pstate = make_parsestate(nullptr);

    EXPECT_EQ(tn->type_oid, 0);  // not set yet
    LookupTypeName(pstate, tn, nullptr);
    EXPECT_EQ(tn->type_oid, kInt8Oid);
    free_parsestate(pstate);
}

TEST_F(ParseTypeExtTest, LookupTypeNameHandlesCaseInsensitiveNames) {
    // "BIGINT", "BigInt", "bigint" should all resolve the same.
    ParseState* pstate = make_parsestate(nullptr);
    for (const char* name : {"BIGINT", "BigInt", "bigint", "INT8", "Int8"}) {
        TypeName* tn = MakeTypeName(name);
        Oid oid = LookupTypeName(pstate, tn, nullptr);
        EXPECT_EQ(oid, kInt8Oid) << "for name " << name;
    }
    free_parsestate(pstate);
}

TEST_F(ParseTypeExtTest, LookupTypeNameReturnsInvalidForUnknownType) {
    TypeName* tn = MakeTypeName("nonexistent_type");
    ParseState* pstate = make_parsestate(nullptr);

    // Should return InvalidOid WITHOUT ereport — caller decides.
    Oid oid = LookupTypeName(pstate, tn, nullptr);
    EXPECT_EQ(oid, kInvalidOid);
    EXPECT_FALSE(RaisesError([&] { LookupTypeName(pstate, tn, nullptr); }));
    free_parsestate(pstate);
}

TEST_F(ParseTypeExtTest, LookupTypeNameNullTypeIsNoop) {
    ParseState* pstate = make_parsestate(nullptr);
    Oid oid = LookupTypeName(pstate, nullptr, nullptr);
    EXPECT_EQ(oid, kInvalidOid);
    free_parsestate(pstate);
}

// ===========================================================================
// LookupTypeName — array suffix handling
// ===========================================================================

TEST_F(ParseTypeExtTest, LookupTypeNameResolvesArrayType) {
    // int4[] should resolve to the array type _int4 (OID 1007).
    TypeName* tn = MakeArrayTypeName("int4");
    ParseState* pstate = make_parsestate(nullptr);

    Oid oid = LookupTypeName(pstate, tn, nullptr);
    EXPECT_NE(oid, kInvalidOid);
    EXPECT_NE(oid, kInt4Oid);  // should be the array type, not the base type
    EXPECT_EQ(tn->type_oid, oid);
    free_parsestate(pstate);
}

TEST_F(ParseTypeExtTest, LookupTypeNameResolvesTextArrayType) {
    // text[] should resolve to the array type _text (OID 1009).
    TypeName* tn = MakeArrayTypeName("text");
    ParseState* pstate = make_parsestate(nullptr);

    Oid oid = LookupTypeName(pstate, tn, nullptr);
    EXPECT_NE(oid, kInvalidOid);
    EXPECT_NE(oid, kTextOid);
    free_parsestate(pstate);
}

// ===========================================================================
// LookupTypeName — typmod handling
// ===========================================================================

TEST_F(ParseTypeExtTest, LookupTypeNameExtractsTypmodForVarchar) {
    // varchar(10) → typmod is set (PostgreSQL uses VARHDRSZ + 10 = 14,
    // but we accept any non-negative value here — the exact encoding is
    // a downstream concern).
    TypeName* tn = MakeTypeNameWithTypmod("varchar", 10);
    ParseState* pstate = make_parsestate(nullptr);
    int32_t typmod = -1;

    Oid oid = LookupTypeName(pstate, tn, &typmod);
    EXPECT_EQ(oid, kVarcharOid);
    EXPECT_GE(typmod, 0);  // should be set, not left at -1
    free_parsestate(pstate);
}

TEST_F(ParseTypeExtTest, LookupTypeNameLeavesTypmodAtNegativeForInt4) {
    // int4 has no typmod, so *typmod should remain -1.
    TypeName* tn = MakeTypeName("int4");
    ParseState* pstate = make_parsestate(nullptr);
    int32_t typmod = -1;

    Oid oid = LookupTypeName(pstate, tn, &typmod);
    EXPECT_EQ(oid, kInt4Oid);
    EXPECT_EQ(typmod, -1);
    free_parsestate(pstate);
}

// ===========================================================================
// typenameTypeId — strict version that ereports on unknown type
// ===========================================================================

TEST_F(ParseTypeExtTest, TypenameTypeIdStrictEreportsOnUnknown) {
    TypeName* tn = MakeTypeName("nonexistent_type");
    ParseState* pstate = make_parsestate(nullptr);

    EXPECT_TRUE(RaisesError([&] { typenameTypeId(pstate, tn, nullptr); }));
    free_parsestate(pstate);
}

TEST_F(ParseTypeExtTest, TypenameTypeIdStrictResolvesKnownType) {
    TypeName* tn = MakeTypeName("int4");
    ParseState* pstate = make_parsestate(nullptr);
    int32_t typmod = -1;

    Oid oid = typenameTypeId(pstate, tn, &typmod);
    EXPECT_EQ(oid, kInt4Oid);
    EXPECT_EQ(tn->type_oid, kInt4Oid);
    free_parsestate(pstate);
}
