#include "mytoydb/common/containers/string_info.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"

namespace {

using mytoydb::containers::appendBinaryStringInfo;
using mytoydb::containers::appendStringInfo;
using mytoydb::containers::appendStringInfoChar;
using mytoydb::containers::appendStringInfoString;
using mytoydb::containers::Data;
using mytoydb::containers::initStringInfo;
using mytoydb::containers::Length;
using mytoydb::containers::makeStringInfo;
using mytoydb::containers::resetStringInfo;
using mytoydb::containers::StringInfo;
using mytoydb::memory::AllocSetContext;

// Helper to properly destroy a palloc'd StringInfo (unregister destructor,
// then call destructor and pfree).
void DestroyStringInfo(StringInfo* si) {
    if (si != nullptr) {
        mytoydb::nodes::destroyPallocNode(si);
    }
}

class StringInfoTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = AllocSetContext::Create("string_info_test_context");
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

// --- Construction and basic state ---

TEST_F(StringInfoTest, DefaultConstructionIsEmpty) {
    StringInfo si;
    EXPECT_TRUE(si.IsEmpty());
    EXPECT_EQ(si.Length(), 0u);
    EXPECT_STREQ(si.Data(), "");
}

TEST_F(StringInfoTest, ConstructWithInitialString) {
    StringInfo si("hello");
    EXPECT_FALSE(si.IsEmpty());
    EXPECT_EQ(si.Length(), 5u);
    EXPECT_STREQ(si.Data(), "hello");
}

// --- AppendString ---

TEST_F(StringInfoTest, AppendString) {
    StringInfo si;
    si.AppendString("hello");
    si.AppendString(" world");
    EXPECT_EQ(si.Length(), 11u);
    EXPECT_STREQ(si.Data(), "hello world");
}

TEST_F(StringInfoTest, AppendStringView) {
    StringInfo si;
    std::string s = "test";
    si.AppendString(s);
    EXPECT_STREQ(si.Data(), "test");
}

// --- AppendChar ---

TEST_F(StringInfoTest, AppendChar) {
    StringInfo si;
    si.AppendChar('a');
    si.AppendChar('b');
    si.AppendChar('c');
    EXPECT_EQ(si.Length(), 3u);
    EXPECT_STREQ(si.Data(), "abc");
}

TEST_F(StringInfoTest, AppendCharMany) {
    StringInfo si;
    for (int i = 0; i < 100; ++i) {
        si.AppendChar(static_cast<char>('A' + (i % 26)));
    }
    EXPECT_EQ(si.Length(), 100u);
    EXPECT_EQ(si.Data()[0], 'A');
    EXPECT_EQ(si.Data()[25], 'Z');
    EXPECT_EQ(si.Data()[26], 'A');
}

// --- AppendPrintf ---

TEST_F(StringInfoTest, AppendPrintfString) {
    StringInfo si;
    si.AppendPrintf("%s", "hello");
    EXPECT_STREQ(si.Data(), "hello");
}

TEST_F(StringInfoTest, AppendPrintfInt) {
    StringInfo si;
    si.AppendPrintf("value = %d", 42);
    EXPECT_STREQ(si.Data(), "value = 42");
}

TEST_F(StringInfoTest, AppendPrintfFloat) {
    StringInfo si;
    si.AppendPrintf("pi = %.2f", 3.14159);
    EXPECT_STREQ(si.Data(), "pi = 3.14");
}

TEST_F(StringInfoTest, AppendPrintfMultiple) {
    StringInfo si;
    si.AppendPrintf("name=%s id=%d score=%.1f", "test", 7, 9.5);
    EXPECT_STREQ(si.Data(), "name=test id=7 score=9.5");
}

TEST_F(StringInfoTest, AppendPrintfAppends) {
    StringInfo si;
    si.AppendString("prefix ");
    si.AppendPrintf("num=%d", 10);
    EXPECT_STREQ(si.Data(), "prefix num=10");
}

// --- AppendBinary ---

TEST_F(StringInfoTest, AppendBinary) {
    StringInfo si;
    const char binary[] = {0x00, 0x01, 0x02, 0x03, 0x04};
    si.AppendBinary(binary, 5);
    EXPECT_EQ(si.Length(), 5u);
    EXPECT_EQ(std::memcmp(si.Data(), binary, 5), 0);
}

TEST_F(StringInfoTest, AppendBinaryWithNullBytes) {
    StringInfo si;
    const char data[] = {'a', '\0', 'b', '\0', 'c'};
    si.AppendBinary(data, 5);
    EXPECT_EQ(si.Length(), 5u);
    EXPECT_EQ(si.Data()[0], 'a');
    EXPECT_EQ(si.Data()[1], '\0');
    EXPECT_EQ(si.Data()[2], 'b');
    EXPECT_EQ(si.Data()[3], '\0');
    EXPECT_EQ(si.Data()[4], 'c');
}

// --- Reset ---

TEST_F(StringInfoTest, Reset) {
    StringInfo si;
    si.AppendString("some data");
    EXPECT_FALSE(si.IsEmpty());
    si.Reset();
    EXPECT_TRUE(si.IsEmpty());
    EXPECT_EQ(si.Length(), 0u);
    EXPECT_STREQ(si.Data(), "");
}

TEST_F(StringInfoTest, ResetAndReuse) {
    StringInfo si;
    si.AppendString("first");
    si.Reset();
    si.AppendString("second");
    EXPECT_STREQ(si.Data(), "second");
    EXPECT_EQ(si.Length(), 6u);
}

// --- Accessors ---

TEST_F(StringInfoTest, MutableData) {
    StringInfo si;
    si.AppendString("hello");
    char* data = si.MutableData();
    data[0] = 'H';
    EXPECT_STREQ(si.Data(), "Hello");
}

TEST_F(StringInfoTest, CapacityIsNonZeroAfterAppend) {
    StringInfo si;
    si.AppendString("hello");
    EXPECT_GE(si.Capacity(), si.Length());
}

TEST_F(StringInfoTest, StrAccessor) {
    StringInfo si;
    si.AppendString("test");
    const std::string& ref = si.Str();
    EXPECT_EQ(ref, "test");
    si.Str() += " more";
    EXPECT_EQ(si.Str(), "test more");
}

// --- Init (no-op with std::string backing) ---

TEST_F(StringInfoTest, InitIsNoOp) {
    StringInfo si;
    si.Init();
    EXPECT_TRUE(si.IsEmpty());
}

// --- PostgreSQL-compatible lowercase API ---

TEST_F(StringInfoTest, MakeStringInfo) {
    StringInfo* si = makeStringInfo();
    ASSERT_NE(si, nullptr);
    EXPECT_TRUE(si->IsEmpty());
    EXPECT_EQ(si->Length(), 0u);
    DestroyStringInfo(si);
}

TEST_F(StringInfoTest, InitStringInfo) {
    StringInfo* si = makeStringInfo();
    initStringInfo(si);
    EXPECT_TRUE(si->IsEmpty());
    DestroyStringInfo(si);
}

TEST_F(StringInfoTest, AppendStringInfo) {
    StringInfo* si = makeStringInfo();
    appendStringInfo(si, "val=%d", 99);
    EXPECT_STREQ(si->Data(), "val=99");
    EXPECT_EQ(si->Length(), 6u);
    DestroyStringInfo(si);
}

TEST_F(StringInfoTest, AppendStringInfoString) {
    StringInfo* si = makeStringInfo();
    appendStringInfoString(si, "hello");
    appendStringInfoString(si, " there");
    EXPECT_STREQ(si->Data(), "hello there");
    DestroyStringInfo(si);
}

TEST_F(StringInfoTest, AppendStringInfoChar) {
    StringInfo* si = makeStringInfo();
    appendStringInfoChar(si, 'x');
    appendStringInfoChar(si, 'y');
    appendStringInfoChar(si, 'z');
    EXPECT_STREQ(si->Data(), "xyz");
    DestroyStringInfo(si);
}

TEST_F(StringInfoTest, AppendBinaryStringInfo) {
    StringInfo* si = makeStringInfo();
    const char data[] = {1, 2, 3, 4};
    appendBinaryStringInfo(si, data, 4);
    EXPECT_EQ(si->Length(), 4u);
    EXPECT_EQ(std::memcmp(si->Data(), data, 4), 0);
    DestroyStringInfo(si);
}

TEST_F(StringInfoTest, ResetStringInfo) {
    StringInfo* si = makeStringInfo();
    appendStringInfoString(si, "data");
    resetStringInfo(si);
    EXPECT_TRUE(si->IsEmpty());
    EXPECT_EQ(si->Length(), 0u);
    DestroyStringInfo(si);
}

TEST_F(StringInfoTest, DataAndLengthFunctions) {
    StringInfo* si = makeStringInfo();
    appendStringInfoString(si, "hello");
    EXPECT_STREQ(Data(si), "hello");
    EXPECT_EQ(Length(si), 5);
    DestroyStringInfo(si);
}

TEST_F(StringInfoTest, DataAndLengthOnNull) {
    EXPECT_EQ(Data(nullptr), nullptr);
    EXPECT_EQ(Length(nullptr), 0);
}

TEST_F(StringInfoTest, AppendStringInfoMultipleFormats) {
    StringInfo* si = makeStringInfo();
    appendStringInfo(si, "name=%s ", "test");
    appendStringInfo(si, "id=%d ", 42);
    appendStringInfo(si, "pi=%.2f", 3.14);
    EXPECT_STREQ(si->Data(), "name=test id=42 pi=3.14");
    DestroyStringInfo(si);
}

}  // namespace
