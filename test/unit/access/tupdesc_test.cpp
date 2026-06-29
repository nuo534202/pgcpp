// tupdesc_test.cpp — Unit tests for tupdesc.c P0 extensions (Task 15.8.2).
//
// Tests CreateTemplateTupleDesc, TupleDescInitEntry, CreateTupleDescCopy,
// CreateTupleDescCopyConstr, TupleDescCopyEntry, FreeTupleDesc, equalTupleDescs,
// and TupleDescInitEntryCollation.
//
// These operations only need a memory context. TupleDescInitEntry resolves
// type metadata via the catalog when available, falling back to hardcoded
// built-in types when the catalog is not set (the test path).

#include <gtest/gtest.h>

#include <string>

#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "types/datum.hpp"

using pgcpp::access::CreateTemplateTupleDesc;
using pgcpp::access::CreateTupleDescCopy;
using pgcpp::access::CreateTupleDescCopyConstr;
using pgcpp::access::equalTupleDescs;
using pgcpp::access::FreeTupleDesc;
using pgcpp::access::TupleDesc;
using pgcpp::access::TupleDescCopyEntry;
using pgcpp::access::TupleDescInitEntry;
using pgcpp::access::TupleDescInitEntryCollation;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt2Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;

namespace {

class TupdescTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        // No catalog set: TupleDescInitEntry uses the hardcoded fallback.
        context_ = AllocSetContext::Create("tupdesc_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

TEST_F(TupdescTest, CreateTemplateTupleDescHasNattsSlots) {
    TupleDesc desc = CreateTemplateTupleDesc(3);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->natts, 3);
    EXPECT_EQ(static_cast<int>(desc->attrs.size()), 3);
    FreeTupleDesc(desc);
}

TEST_F(TupdescTest, TupleDescInitEntryInt4) {
    TupleDesc desc = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(desc, 1, "id", kInt4Oid, -1, 0);
    const auto& attr = desc->attrs[0];
    EXPECT_EQ(attr.attname, "id");
    EXPECT_EQ(attr.atttypid, kInt4Oid);
    EXPECT_EQ(attr.attnum, 1);
    EXPECT_EQ(attr.attlen, 4);
    EXPECT_TRUE(attr.attbyval);
    EXPECT_EQ(attr.attalign, AttAlign::kInt);
    FreeTupleDesc(desc);
}

TEST_F(TupdescTest, TupleDescInitEntryInt8AlignsToDouble) {
    TupleDesc desc = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(desc, 1, "big", kInt8Oid, -1, 0);
    EXPECT_EQ(desc->attrs[0].attlen, 8);
    EXPECT_EQ(desc->attrs[0].attalign, AttAlign::kDouble);
    FreeTupleDesc(desc);
}

TEST_F(TupdescTest, TupleDescInitEntryBoolAlignsToChar) {
    TupleDesc desc = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(desc, 1, "flag", kBoolOid, -1, 0);
    EXPECT_EQ(desc->attrs[0].attlen, 1);
    EXPECT_EQ(desc->attrs[0].attalign, AttAlign::kChar);
    FreeTupleDesc(desc);
}

TEST_F(TupdescTest, TupleDescInitEntryTextIsVarlena) {
    TupleDesc desc = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(desc, 1, "txt", kTextOid, -1, 0);
    EXPECT_EQ(desc->attrs[0].attlen, -1);
    EXPECT_FALSE(desc->attrs[0].attbyval);
    FreeTupleDesc(desc);
}

TEST_F(TupdescTest, TupleDescInitEntryCollation) {
    TupleDesc desc = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(desc, 1, "txt", kTextOid, -1, 0);
    constexpr Oid kCollationOid = 100;  // default collation OID (arbitrary for test)
    TupleDescInitEntryCollation(desc, 1, kCollationOid);
    EXPECT_EQ(desc->attrs[0].attcollation, kCollationOid);
    FreeTupleDesc(desc);
}

TEST_F(TupdescTest, CreateTupleDescCopyDuplicatesAttrs) {
    TupleDesc desc = CreateTemplateTupleDesc(2);
    TupleDescInitEntry(desc, 1, "a", kInt4Oid, -1, 0);
    TupleDescInitEntry(desc, 2, "b", kInt2Oid, -1, 0);
    desc->tdtypeid = 42;

    TupleDesc copy = CreateTupleDescCopy(desc);
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->natts, 2);
    EXPECT_EQ(copy->attrs[0].attname, "a");
    EXPECT_EQ(copy->attrs[1].attname, "b");
    EXPECT_EQ(copy->tdtypeid, 42u);
    // Copy does not duplicate constraints (PG semantics).
    EXPECT_EQ(copy->constr.defval.size(), 0u);

    FreeTupleDesc(copy);
    FreeTupleDesc(desc);
}

TEST_F(TupdescTest, CreateTupleDescCopyConstrDuplicatesConstraints) {
    TupleDesc desc = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(desc, 1, "a", kInt4Oid, -1, 0);
    desc->constr.has_not_null = true;
    desc->constr.defval.push_back({1, "0"});

    TupleDesc copy = CreateTupleDescCopyConstr(desc);
    ASSERT_NE(copy, nullptr);
    EXPECT_TRUE(copy->constr.has_not_null);
    EXPECT_EQ(copy->constr.defval.size(), 1u);
    EXPECT_EQ(copy->constr.defval[0].adbin, "0");

    FreeTupleDesc(copy);
    FreeTupleDesc(desc);
}

TEST_F(TupdescTest, TupleDescCopyEntryCopiesSlot) {
    TupleDesc src = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(src, 1, "src_col", kInt4Oid, -1, 0);

    TupleDesc dst = CreateTemplateTupleDesc(1);
    TupleDescCopyEntry(dst, 1, src, 1);
    EXPECT_EQ(dst->attrs[0].attname, "src_col");
    EXPECT_EQ(dst->attrs[0].atttypid, kInt4Oid);
    // attnum is updated to match the destination slot.
    EXPECT_EQ(dst->attrs[0].attnum, 1);

    FreeTupleDesc(dst);
    FreeTupleDesc(src);
}

TEST_F(TupdescTest, EqualTupleDescsIdentical) {
    TupleDesc d1 = CreateTemplateTupleDesc(2);
    TupleDescInitEntry(d1, 1, "a", kInt4Oid, -1, 0);
    TupleDescInitEntry(d1, 2, "b", kInt2Oid, -1, 0);

    TupleDesc d2 = CreateTemplateTupleDesc(2);
    TupleDescInitEntry(d2, 1, "a", kInt4Oid, -1, 0);
    TupleDescInitEntry(d2, 2, "b", kInt2Oid, -1, 0);

    EXPECT_TRUE(equalTupleDescs(d1, d2));
    EXPECT_TRUE(equalTupleDescs(d1, d1));  // self-equal (same pointer).

    FreeTupleDesc(d1);
    FreeTupleDesc(d2);
}

TEST_F(TupdescTest, EqualTupleDescsDifferByAttrName) {
    TupleDesc d1 = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(d1, 1, "a", kInt4Oid, -1, 0);

    TupleDesc d2 = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(d2, 1, "x", kInt4Oid, -1, 0);

    EXPECT_FALSE(equalTupleDescs(d1, d2));

    FreeTupleDesc(d1);
    FreeTupleDesc(d2);
}

TEST_F(TupdescTest, EqualTupleDescsDifferByNatts) {
    TupleDesc d1 = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(d1, 1, "a", kInt4Oid, -1, 0);

    TupleDesc d2 = CreateTemplateTupleDesc(2);
    TupleDescInitEntry(d2, 1, "a", kInt4Oid, -1, 0);
    TupleDescInitEntry(d2, 2, "b", kInt4Oid, -1, 0);

    EXPECT_FALSE(equalTupleDescs(d1, d2));

    FreeTupleDesc(d1);
    FreeTupleDesc(d2);
}

TEST_F(TupdescTest, FreeTupleDescRespectsRefcount) {
    TupleDesc desc = CreateTemplateTupleDesc(1);
    // tdrefcount starts at 0; FreeTupleDesc frees immediately.
    desc->tdrefcount = 1;
    FreeTupleDesc(desc);  // decrements to 0 but does NOT free (refcount was > 0).

    // Now refcount is 0; a second FreeTupleDesc frees it.
    FreeTupleDesc(desc);
    // desc is now freed; the test passes if no crash / leak.
}

}  // namespace
