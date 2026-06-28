#include "pgcpp/common/containers/node.hpp"

#include <gtest/gtest.h>

#include <string>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace {

using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::copyObject;
using mytoydb::nodes::equal;
using mytoydb::nodes::isA;
using mytoydb::nodes::makeFloat;
using mytoydb::nodes::makeInteger;
using mytoydb::nodes::makeNull;
using mytoydb::nodes::makeString;
using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;
using mytoydb::nodes::Value;

// Test fixture that sets up a memory context (same pattern as elog_test.cpp).
// All Node allocations via palloc land in this context.
class NodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("node_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

// --- Value integer ---------------------------------------------------------

TEST_F(NodeTest, ValueIntegerCreation) {
    Value v(static_cast<int64_t>(42));
    EXPECT_EQ(v.GetInteger(), 42);
    EXPECT_EQ(v.GetTag(), NodeTag::kInteger);
    EXPECT_FALSE(v.IsNull());
}

TEST_F(NodeTest, ValueIntegerNegative) {
    Value v(static_cast<int64_t>(-7));
    EXPECT_EQ(v.GetInteger(), -7);
    EXPECT_EQ(v.GetTag(), NodeTag::kInteger);
}

// --- Value float -----------------------------------------------------------

TEST_F(NodeTest, ValueFloatCreation) {
    Value v(std::string("3.14"));
    EXPECT_EQ(v.GetFloat(), "3.14");
    EXPECT_EQ(v.GetTag(), NodeTag::kFloat);
    EXPECT_FALSE(v.IsNull());
}

// --- Value string ----------------------------------------------------------

TEST_F(NodeTest, ValueStringCreation) {
    Value v(std::string("hello"), true);
    EXPECT_EQ(v.GetString(), "hello");
    EXPECT_EQ(v.GetTag(), NodeTag::kString);
    EXPECT_FALSE(v.IsNull());
}

// --- Value null ------------------------------------------------------------

TEST_F(NodeTest, ValueNullCreation) {
    Value v;
    EXPECT_TRUE(v.IsNull());
    EXPECT_EQ(v.GetTag(), NodeTag::kNull);
}

// --- makeInteger / makeFloat / makeString / makeNull -----------------------

TEST_F(NodeTest, MakeIntegerWorks) {
    Value* v = makeInteger(123);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->GetInteger(), 123);
    EXPECT_EQ(v->GetTag(), NodeTag::kInteger);
}

TEST_F(NodeTest, MakeFloatWorks) {
    Value* v = makeFloat(std::string("2.71828"));
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->GetFloat(), "2.71828");
    EXPECT_EQ(v->GetTag(), NodeTag::kFloat);
}

TEST_F(NodeTest, MakeStringWorks) {
    Value* v = makeString(std::string("world"));
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->GetString(), "world");
    EXPECT_EQ(v->GetTag(), NodeTag::kString);
}

TEST_F(NodeTest, MakeNullWorks) {
    Value* v = makeNull();
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(v->IsNull());
    EXPECT_EQ(v->GetTag(), NodeTag::kNull);
}

// --- nodeTag() -------------------------------------------------------------

TEST_F(NodeTest, NodeTagReturnsCorrectTag) {
    Value* vi = makeInteger(1);
    Value* vf = makeFloat(std::string("1.0"));
    Value* vs = makeString(std::string("x"));
    Value* vn = makeNull();
    EXPECT_EQ(nodeTag(vi), NodeTag::kInteger);
    EXPECT_EQ(nodeTag(vf), NodeTag::kFloat);
    EXPECT_EQ(nodeTag(vs), NodeTag::kString);
    EXPECT_EQ(nodeTag(vn), NodeTag::kNull);
}

TEST_F(NodeTest, NodeTagNullPtrReturnsInvalid) {
    EXPECT_EQ(nodeTag(nullptr), NodeTag::kInvalid);
}

// --- isA() -----------------------------------------------------------------

TEST_F(NodeTest, IsATypeCheckWorks) {
    Value* vi = makeInteger(1);
    Value* vs = makeString(std::string("x"));
    EXPECT_TRUE(isA(vi, NodeTag::kInteger));
    EXPECT_FALSE(isA(vi, NodeTag::kString));
    EXPECT_TRUE(isA(vs, NodeTag::kString));
    EXPECT_FALSE(isA(vs, NodeTag::kInteger));
}

TEST_F(NodeTest, IsANullPtrReturnsFalse) {
    EXPECT_FALSE(isA(nullptr, NodeTag::kInteger));
}

// --- copyObject() ----------------------------------------------------------

TEST_F(NodeTest, CopyObjectDeepCopiesInteger) {
    Value* original = makeInteger(99);
    Node* copy = copyObject(original);
    ASSERT_NE(copy, nullptr);
    EXPECT_NE(copy, original);  // distinct objects
    ASSERT_EQ(copy->GetTag(), NodeTag::kInteger);
    auto* copy_val = static_cast<Value*>(copy);
    EXPECT_EQ(copy_val->GetInteger(), 99);
    // Original is unchanged.
    EXPECT_EQ(original->GetInteger(), 99);
}

TEST_F(NodeTest, CopyObjectDeepCopiesString) {
    Value* original = makeString(std::string("deep"));
    Node* copy = copyObject(original);
    ASSERT_NE(copy, nullptr);
    EXPECT_NE(copy, original);  // distinct objects, distinct memory
    ASSERT_EQ(copy->GetTag(), NodeTag::kString);
    auto* copy_val = static_cast<Value*>(copy);
    EXPECT_EQ(copy_val->GetString(), "deep");
    // Original is unchanged.
    EXPECT_EQ(original->GetString(), "deep");
}

TEST_F(NodeTest, CopyObjectNullptrReturnsNullptr) {
    EXPECT_EQ(copyObject(nullptr), nullptr);
}

// --- equal() ---------------------------------------------------------------

TEST_F(NodeTest, EqualIntegersEqual) {
    Value* a = makeInteger(5);
    Value* b = makeInteger(5);
    EXPECT_TRUE(equal(a, b));
}

TEST_F(NodeTest, EqualIntegersNotEqual) {
    Value* a = makeInteger(5);
    Value* b = makeInteger(6);
    EXPECT_FALSE(equal(a, b));
}

TEST_F(NodeTest, EqualFloatsEqual) {
    Value* a = makeFloat(std::string("1.5"));
    Value* b = makeFloat(std::string("1.5"));
    EXPECT_TRUE(equal(a, b));
}

TEST_F(NodeTest, EqualFloatsNotEqual) {
    Value* a = makeFloat(std::string("1.5"));
    Value* b = makeFloat(std::string("2.5"));
    EXPECT_FALSE(equal(a, b));
}

TEST_F(NodeTest, EqualStringsEqual) {
    Value* a = makeString(std::string("abc"));
    Value* b = makeString(std::string("abc"));
    EXPECT_TRUE(equal(a, b));
}

TEST_F(NodeTest, EqualStringsNotEqual) {
    Value* a = makeString(std::string("abc"));
    Value* b = makeString(std::string("abd"));
    EXPECT_FALSE(equal(a, b));
}

TEST_F(NodeTest, EqualNullsEqual) {
    Value* a = makeNull();
    Value* b = makeNull();
    EXPECT_TRUE(equal(a, b));
}

TEST_F(NodeTest, EqualDifferentTagsNotEqual) {
    Value* a = makeInteger(5);
    Value* b = makeString(std::string("5"));
    EXPECT_FALSE(equal(a, b));
}

TEST_F(NodeTest, EqualBothNullptrReturnsTrue) {
    EXPECT_TRUE(equal(nullptr, nullptr));
}

TEST_F(NodeTest, EqualOneNullptrReturnsFalse) {
    Value* a = makeInteger(1);
    EXPECT_FALSE(equal(a, nullptr));
    EXPECT_FALSE(equal(nullptr, a));
}

TEST_F(NodeTest, EqualSamePointerReturnsTrue) {
    Value* a = makeInteger(7);
    EXPECT_TRUE(equal(a, a));
}

}  // namespace
