// lsyscache_test.cpp — unit tests for the lsyscache convenience layer
// (M3 Task 15.6.2).
//
// Verifies every P0 lsyscache function against manually-inserted catalog
// rows. Missing rows must return InvalidOid / nullptr / false (no ereport).
#include "mytoydb/catalog/lsyscache.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_attribute.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/catalog/pg_operator.hpp"
#include "mytoydb/catalog/pg_proc.hpp"
#include "mytoydb/catalog/pg_type.hpp"
#include "mytoydb/catalog/syscache.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

namespace {

using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::FormData_pg_operator;
using mytoydb::catalog::FormData_pg_proc;
using mytoydb::catalog::FormData_pg_type;
using mytoydb::catalog::get_att;
using mytoydb::catalog::get_attname;
using mytoydb::catalog::get_attnotnull;
using mytoydb::catalog::get_attnum;
using mytoydb::catalog::get_atttype;
using mytoydb::catalog::get_commutator;
using mytoydb::catalog::get_func_name;
using mytoydb::catalog::get_func_nargs;
using mytoydb::catalog::get_func_prokind;
using mytoydb::catalog::get_func_rettype;
using mytoydb::catalog::get_negator;
using mytoydb::catalog::get_op;
using mytoydb::catalog::get_opcode;
using mytoydb::catalog::get_opname;
using mytoydb::catalog::get_rel_name;
using mytoydb::catalog::get_rel_namespace;
using mytoydb::catalog::get_rel_persistence;
using mytoydb::catalog::get_rel_relkind;
using mytoydb::catalog::get_typalign;
using mytoydb::catalog::get_typbyval;
using mytoydb::catalog::get_typcategory;
using mytoydb::catalog::get_type_name;
using mytoydb::catalog::get_typelem;
using mytoydb::catalog::get_typisdefined;
using mytoydb::catalog::get_typlen;
using mytoydb::catalog::get_typstorage;
using mytoydb::catalog::kInvalidAttrNumber;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::op_mergejoinable;
using mytoydb::catalog::op_strict;
using mytoydb::catalog::OperatorKind;
using mytoydb::catalog::ProKind;
using mytoydb::catalog::RelKind;
using mytoydb::catalog::RelPersistence;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::catalog::type_is_enum;
using mytoydb::catalog::type_is_rowtype;
using mytoydb::catalog::TypeAlign;
using mytoydb::catalog::TypeCategory;
using mytoydb::catalog::TypeStorage;
using mytoydb::catalog::TypeType;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::makePallocNode;

// Test OID constants — chosen to not collide with bootstrap OIDs (< 16384).
constexpr Oid kOpOid = 21000;     // "=" operator (int4 = int4)
constexpr Oid kOpFuncOid = 350;   // int4eq function OID
constexpr Oid kOpComm = 21001;    // commutator OID
constexpr Oid kOpNegate = 21002;  // negator OID
constexpr Oid kFuncOid = 22000;   // test function OID
constexpr Oid kTypeInt4 = 23;
constexpr Oid kTypeText = 25;
constexpr Oid kTypeInt8 = 20;
constexpr Oid kRelid = 20000;

class LsyscacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("lsyscache_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        // --- pg_operator row for "=" on int4 ---
        auto* op = makePallocNode<FormData_pg_operator>();
        op->oid = kOpOid;
        op->oprname = "=";
        op->oprkind = OperatorKind::kBinary;
        op->oprleft = kTypeInt4;
        op->oprright = kTypeInt4;
        op->oprresult = 16;  // bool
        op->oprcom = kOpComm;
        op->oprnegate = kOpNegate;
        op->oprcode = kOpFuncOid;
        op->oprcanmerge = true;
        op->oprcanhash = false;
        catalog_->InsertOperator(op);

        // --- pg_proc rows: implementation function (int4eq) + a test function ---
        auto* proc_eq = makePallocNode<FormData_pg_proc>();
        proc_eq->oid = kOpFuncOid;
        proc_eq->proname = "int4eq";
        proc_eq->prokind = ProKind::kFunction;
        proc_eq->proisstrict = true;
        proc_eq->pronargs = 2;
        proc_eq->prorettype = 16;  // bool
        proc_eq->proargtypes = {kTypeInt4, kTypeInt4};
        catalog_->InsertProc(proc_eq);

        auto* proc_test = makePallocNode<FormData_pg_proc>();
        proc_test->oid = kFuncOid;
        proc_test->proname = "my_func";
        proc_test->prokind = ProKind::kFunction;
        proc_test->proisstrict = false;
        proc_test->pronargs = 1;
        proc_test->prorettype = kTypeText;
        proc_test->proargtypes = {kTypeInt4};
        catalog_->InsertProc(proc_test);

        // --- pg_type rows for int4 and text ---
        auto* type_int4 = makePallocNode<FormData_pg_type>();
        type_int4->oid = kTypeInt4;
        type_int4->typname = "int4";
        type_int4->typlen = 4;
        type_int4->typbyval = true;
        type_int4->typtype = TypeType::kBase;
        type_int4->typcategory = TypeCategory::kNumeric;
        type_int4->typisdefined = true;
        type_int4->typalign = TypeAlign::kInt;
        type_int4->typstorage = TypeStorage::kPlain;
        type_int4->typelem = kInvalidOid;
        catalog_->InsertType(type_int4);

        auto* type_text = makePallocNode<FormData_pg_type>();
        type_text->oid = kTypeText;
        type_text->typname = "text";
        type_text->typlen = -1;  // varlena
        type_text->typbyval = false;
        type_text->typtype = TypeType::kBase;
        type_text->typcategory = TypeCategory::kString;
        type_text->typisdefined = true;
        type_text->typalign = TypeAlign::kInt;
        type_text->typstorage = TypeStorage::kExtended;
        type_text->typelem = kInvalidOid;
        catalog_->InsertType(type_text);

        // --- pg_class row for a "hits" relation ---
        auto* cls = makePallocNode<FormData_pg_class>();
        cls->oid = kRelid;
        cls->relname = "hits";
        cls->relkind = RelKind::kRelation;
        cls->relpersistence = RelPersistence::kPermanent;
        cls->relnamespace = 2200;  // PG's "public" namespace OID
        cls->relnatts = 2;
        catalog_->InsertClass(cls);

        // --- pg_attribute rows ---
        InsertAttribute(kRelid, "id", 1, kTypeInt4, /*notnull=*/true);
        InsertAttribute(kRelid, "url", 2, kTypeText, /*notnull=*/false);
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

    void InsertAttribute(Oid relid, const std::string& name, int16_t attnum, Oid typid,
                         bool notnull) {
        auto* row = makePallocNode<FormData_pg_attribute>();
        row->attrelid = relid;
        row->attname = name;
        row->attnum = attnum;
        row->atttypid = typid;
        row->attnotnull = notnull;
        catalog_->InsertAttribute(row);
    }

    // Helper: free a palloc'd char* (called inside the memory context).
    static void FreeChar(char* p) {
        if (p != nullptr) {
            mytoydb::memory::pfree(p);
        }
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// ===========================================================================
// pg_operator lookups
// ===========================================================================

TEST_F(LsyscacheTest, get_opcode_ReturnsImplFuncOid) {
    EXPECT_EQ(get_opcode(kOpOid), kOpFuncOid);
}

TEST_F(LsyscacheTest, get_opcode_ReturnsInvalidForMissingOid) {
    EXPECT_EQ(get_opcode(99999), kInvalidOid);
}

TEST_F(LsyscacheTest, get_op_ReturnsFullRow) {
    const auto* op = get_op(kOpOid);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->oprname, "=");
    EXPECT_EQ(op->oprleft, kTypeInt4);
}

TEST_F(LsyscacheTest, get_op_ReturnsNullForMissingOid) {
    EXPECT_EQ(get_op(99999), nullptr);
}

TEST_F(LsyscacheTest, get_opname_ReturnsName) {
    char* name = get_opname(kOpOid);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "=");
    FreeChar(name);
}

TEST_F(LsyscacheTest, get_opname_ReturnsNullForMissingOid) {
    EXPECT_EQ(get_opname(99999), nullptr);
}

TEST_F(LsyscacheTest, get_commutator_ReturnsCommOid) {
    EXPECT_EQ(get_commutator(kOpOid), kOpComm);
}

TEST_F(LsyscacheTest, get_negator_ReturnsNegOid) {
    EXPECT_EQ(get_negator(kOpOid), kOpNegate);
}

TEST_F(LsyscacheTest, op_mergejoinable_TrueForMergeOp) {
    Oid left = 0, right = 0;
    EXPECT_TRUE(op_mergejoinable(kOpOid, &left, &right));
    EXPECT_EQ(left, kTypeInt4);
    EXPECT_EQ(right, kTypeInt4);
}

TEST_F(LsyscacheTest, op_strict_TrueForStrictImpl) {
    EXPECT_TRUE(op_strict(kOpOid));
}

TEST_F(LsyscacheTest, op_strict_FalseForMissingOp) {
    EXPECT_FALSE(op_strict(99999));
}

// ===========================================================================
// pg_proc lookups
// ===========================================================================

TEST_F(LsyscacheTest, get_func_rettype) {
    EXPECT_EQ(get_func_rettype(kFuncOid), kTypeText);
    EXPECT_EQ(get_func_rettype(kOpFuncOid), static_cast<Oid>(16));
}

TEST_F(LsyscacheTest, get_func_rettype_InvalidForMissing) {
    EXPECT_EQ(get_func_rettype(99999), kInvalidOid);
}

TEST_F(LsyscacheTest, get_func_name) {
    char* name = get_func_name(kFuncOid);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "my_func");
    FreeChar(name);
}

TEST_F(LsyscacheTest, get_func_name_NullForMissing) {
    EXPECT_EQ(get_func_name(99999), nullptr);
}

TEST_F(LsyscacheTest, get_func_prokind) {
    EXPECT_EQ(get_func_prokind(kFuncOid), static_cast<char>(ProKind::kFunction));
}

TEST_F(LsyscacheTest, get_func_prokind_NullCharForMissing) {
    EXPECT_EQ(get_func_prokind(99999), '\0');
}

TEST_F(LsyscacheTest, get_func_nargs) {
    EXPECT_EQ(get_func_nargs(kFuncOid), 1);
    EXPECT_EQ(get_func_nargs(kOpFuncOid), 2);
}

TEST_F(LsyscacheTest, get_func_nargs_NegOneForMissing) {
    EXPECT_EQ(get_func_nargs(99999), -1);
}

// ===========================================================================
// pg_type lookups
// ===========================================================================

TEST_F(LsyscacheTest, get_type_name) {
    char* name = get_type_name(kTypeInt4);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "int4");
    FreeChar(name);
}

TEST_F(LsyscacheTest, get_type_name_NullForMissing) {
    EXPECT_EQ(get_type_name(99999), nullptr);
}

TEST_F(LsyscacheTest, get_typlen) {
    EXPECT_EQ(get_typlen(kTypeInt4), 4);
    EXPECT_EQ(get_typlen(kTypeText), -1);  // varlena
    EXPECT_EQ(get_typlen(99999), 0);
}

TEST_F(LsyscacheTest, get_typbyval) {
    EXPECT_TRUE(get_typbyval(kTypeInt4));
    EXPECT_FALSE(get_typbyval(kTypeText));
    EXPECT_FALSE(get_typbyval(99999));
}

TEST_F(LsyscacheTest, get_typalign) {
    EXPECT_EQ(get_typalign(kTypeInt4), static_cast<char>(TypeAlign::kInt));
    EXPECT_EQ(get_typalign(99999), '\0');
}

TEST_F(LsyscacheTest, get_typstorage) {
    EXPECT_EQ(get_typstorage(kTypeInt4), static_cast<char>(TypeStorage::kPlain));
    EXPECT_EQ(get_typstorage(kTypeText), static_cast<char>(TypeStorage::kExtended));
    EXPECT_EQ(get_typstorage(99999), '\0');
}

TEST_F(LsyscacheTest, get_typcategory) {
    EXPECT_EQ(get_typcategory(kTypeInt4), static_cast<char>(TypeCategory::kNumeric));
    EXPECT_EQ(get_typcategory(kTypeText), static_cast<char>(TypeCategory::kString));
}

TEST_F(LsyscacheTest, get_typisdefined) {
    EXPECT_TRUE(get_typisdefined(kTypeInt4));
    EXPECT_FALSE(get_typisdefined(99999));
}

TEST_F(LsyscacheTest, get_typelem) {
    EXPECT_EQ(get_typelem(kTypeInt4), kInvalidOid);  // not an array type
    EXPECT_EQ(get_typelem(99999), kInvalidOid);
}

// ===========================================================================
// pg_attribute lookups
// ===========================================================================

TEST_F(LsyscacheTest, get_att) {
    const auto* attr = get_att(kRelid, "id");
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(attr->attnum, 1);
    EXPECT_EQ(attr->atttypid, kTypeInt4);
}

TEST_F(LsyscacheTest, get_att_NullForMissingName) {
    EXPECT_EQ(get_att(kRelid, "nonexistent"), nullptr);
    EXPECT_EQ(get_att(kRelid, nullptr), nullptr);
}

TEST_F(LsyscacheTest, get_attname) {
    char* name = get_attname(kRelid, 1);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "id");
    FreeChar(name);

    char* name2 = get_attname(kRelid, 2);
    ASSERT_NE(name2, nullptr);
    EXPECT_STREQ(name2, "url");
    FreeChar(name2);
}

TEST_F(LsyscacheTest, get_attname_NullForMissing) {
    EXPECT_EQ(get_attname(kRelid, 99), nullptr);
    EXPECT_EQ(get_attname(99999, 1), nullptr);
}

TEST_F(LsyscacheTest, get_atttype) {
    EXPECT_EQ(get_atttype(kRelid, 1), kTypeInt4);
    EXPECT_EQ(get_atttype(kRelid, 2), kTypeText);
    EXPECT_EQ(get_atttype(kRelid, 99), kInvalidOid);
}

TEST_F(LsyscacheTest, get_attnum) {
    EXPECT_EQ(get_attnum(kRelid, "id"), 1);
    EXPECT_EQ(get_attnum(kRelid, "url"), 2);
    EXPECT_EQ(get_attnum(kRelid, "missing"), kInvalidAttrNumber);
}

TEST_F(LsyscacheTest, get_attnotnull) {
    EXPECT_TRUE(get_attnotnull(kRelid, 1));   // id is NOT NULL
    EXPECT_FALSE(get_attnotnull(kRelid, 2));  // url is nullable
    EXPECT_FALSE(get_attnotnull(kRelid, 99));
}

// ===========================================================================
// pg_class lookups
// ===========================================================================

TEST_F(LsyscacheTest, get_rel_name) {
    char* name = get_rel_name(kRelid);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "hits");
    FreeChar(name);
}

TEST_F(LsyscacheTest, get_rel_name_NullForMissing) {
    EXPECT_EQ(get_rel_name(99999), nullptr);
}

TEST_F(LsyscacheTest, get_rel_relkind) {
    EXPECT_EQ(get_rel_relkind(kRelid), static_cast<char>(RelKind::kRelation));
    EXPECT_EQ(get_rel_relkind(99999), '\0');
}

TEST_F(LsyscacheTest, get_rel_persistence) {
    EXPECT_EQ(get_rel_persistence(kRelid), static_cast<char>(RelPersistence::kPermanent));
    EXPECT_EQ(get_rel_persistence(99999), '\0');
}

TEST_F(LsyscacheTest, get_rel_namespace) {
    EXPECT_EQ(get_rel_namespace(kRelid), static_cast<Oid>(2200));
    EXPECT_EQ(get_rel_namespace(99999), kInvalidOid);
}

// ===========================================================================
// Predicates
// ===========================================================================

TEST_F(LsyscacheTest, type_is_rowtype_FalseForBaseType) {
    EXPECT_FALSE(type_is_rowtype(kTypeInt4));
    EXPECT_FALSE(type_is_rowtype(99999));
}

TEST_F(LsyscacheTest, type_is_enum_AlwaysFalse) {
    EXPECT_FALSE(type_is_enum(kTypeInt4));
    EXPECT_FALSE(type_is_enum(99999));
}

}  // namespace
