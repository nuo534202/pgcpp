#include "common/error/elog.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace {

// Tracker class whose destructor increments a global counter. Used to verify
// that C++ stack unwinding calls destructors when ereport(ERROR) throws
// PgException — the core fix of A-5 (longjmp bypassed destructors → UB/leak).
int g_destructor_calls = 0;
struct TrackDestructor {
    ~TrackDestructor() { ++g_destructor_calls; }
};

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;

class ElogTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("test_context");
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

// WARNING level should NOT longjmp — it returns normally.
TEST_F(ElogTest, WarningDoesNotLongjmp) {
    bool reached_after = false;
    ereport(LogLevel::kWarning, "this is a warning");
    reached_after = true;
    EXPECT_TRUE(reached_after);
}

// INFO level should NOT longjmp.
TEST_F(ElogTest, InfoDoesNotLongjmp) {
    ereport(LogLevel::kInfo, "informational message");
    SUCCEED();
}

// NOTICE level should NOT longjmp.
TEST_F(ElogTest, NoticeDoesNotLongjmp) {
    ereport(LogLevel::kNotice, "notice message");
    SUCCEED();
}

// ERROR inside PG_TRY should longjmp to PG_CATCH.
TEST_F(ElogTest, ErrorInPgTryIsCaught) {
    bool caught = false;
    PG_TRY() {
        ereport(LogLevel::kError, "test error");
        FAIL() << "Should not reach here";
    }
    PG_CATCH() {
        caught = true;
        ErrorData* err = pgcpp::error::GetErrorData();
        EXPECT_EQ(err->elevel, LogLevel::kError);
        EXPECT_EQ(err->message, "test error");
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
}

// Code after PG_END_TRY continues normally after a caught error.
TEST_F(ElogTest, ContinuesAfterPgEndTry) {
    bool continued = false;
    PG_TRY() {
        ereport(LogLevel::kError, "error then continue");
    }
    PG_CATCH() {
        // handled
    }
    PG_END_TRY();
    continued = true;
    EXPECT_TRUE(continued);
}

// elog alias works the same as ereport.
TEST_F(ElogTest, ElogAliasWorks) {
    bool caught = false;
    PG_TRY() {
        elog(LogLevel::kError, "elog alias test");
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
}

// Nested PG_TRY blocks: inner catch handles inner error, outer continues.
TEST_F(ElogTest, NestedPgTry) {
    bool outer_continued = false;
    bool inner_caught = false;

    PG_TRY() {
        PG_TRY() {
            ereport(LogLevel::kError, "inner error");
        }
        PG_CATCH() {
            inner_caught = true;
        }
        PG_END_TRY();
        outer_continued = true;
    }
    PG_CATCH() {
        FAIL() << "Outer catch should not be reached";
    }
    PG_END_TRY();

    EXPECT_TRUE(inner_caught);
    EXPECT_TRUE(outer_continued);
}

// Error data is populated correctly.
TEST_F(ElogTest, ErrorDataPopulated) {
    PG_TRY() {
        ereport(LogLevel::kError, "populated error data");
    }
    PG_CATCH() {
        ErrorData* err = pgcpp::error::GetErrorData();
        EXPECT_EQ(err->elevel, LogLevel::kError);
        EXPECT_EQ(err->message, "populated error data");
        EXPECT_NE(err->filename, nullptr);
        EXPECT_GT(err->lineno, 0);
    }
    PG_END_TRY();
}

// FATAL level is also caught by PG_CATCH.
TEST_F(ElogTest, FatalIsCaught) {
    bool caught = false;
    PG_TRY() {
        ereport(LogLevel::kFatal, "fatal error test");
    }
    PG_CATCH() {
        caught = true;
        ErrorData* err = pgcpp::error::GetErrorData();
        EXPECT_EQ(err->elevel, LogLevel::kFatal);
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
}

// Multiple sequential errors in the same PG_TRY block are not possible
// (the first error longjmps), but multiple PG_TRY blocks in sequence work.
TEST_F(ElogTest, SequentialPgTryBlocks) {
    int catch_count = 0;
    for (int i = 0; i < 3; ++i) {
        PG_TRY() {
            ereport(LogLevel::kError, "sequential error");
        }
        PG_CATCH() {
            ++catch_count;
        }
        PG_END_TRY();
    }
    EXPECT_EQ(catch_count, 3);
}

// Error after a successful non-error ereport in the same block.
TEST_F(ElogTest, WarningThenError) {
    bool caught = false;
    PG_TRY() {
        ereport(LogLevel::kWarning, "warning first");
        ereport(LogLevel::kError, "then error");
    }
    PG_CATCH() {
        caught = true;
        ErrorData* err = pgcpp::error::GetErrorData();
        EXPECT_EQ(err->message, "then error");
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
}

// --- A-5 tests: stack unwinding calls destructors ---

// ereport(ERROR) inside a PG_TRY should call destructors for local
// variables in the try block before reaching PG_CATCH. This is the core
// fix of A-5 — longjmp bypassed these destructors (UB + leak).
TEST_F(ElogTest, StackUnwindingCallsLocalDestructor) {
    g_destructor_calls = 0;
    bool caught = false;
    PG_TRY() {
        TrackDestructor tracker;  // destructor should run during unwinding
        ereport(LogLevel::kError, "unwind test");
        FAIL() << "Should not reach here";
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
    EXPECT_EQ(g_destructor_calls, 1) << "Destructor was not called during stack unwinding";
}

// std::string locals (heap-allocated buffer) should be properly freed
// during stack unwinding — no leak. Under ASan/LSan this would be caught
// as a leak if the destructor were bypassed (the old longjmp bug).
TEST_F(ElogTest, StdStringLocalFreedDuringUnwinding) {
    bool caught = false;
    PG_TRY() {
        std::string local_string(1024, 'x');  // forces heap allocation
        ereport(LogLevel::kError, "string unwind test");
        FAIL() << "Should not reach here";
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
    // If the string's destructor ran during unwinding, its heap buffer
    // was freed. ASan/LSan will report a leak if it wasn't.
}

// std::vector locals should also be properly freed during unwinding.
TEST_F(ElogTest, StdVectorLocalFreedDuringUnwinding) {
    bool caught = false;
    PG_TRY() {
        std::vector<int> local_vec(1024, 42);  // forces heap allocation
        ereport(LogLevel::kError, "vector unwind test");
        FAIL() << "Should not reach here";
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
    // If the vector's destructor ran during unwinding, its heap buffer
    // was freed. ASan/LSan will report a leak if it wasn't.
}

// Multiple locals at different scopes should all be destructed.
TEST_F(ElogTest, MultipleLocalsAllDestructed) {
    g_destructor_calls = 0;
    bool caught = false;
    PG_TRY() {
        TrackDestructor t1;
        {
            TrackDestructor t2;
            TrackDestructor t3;
        }  // t2, t3 destructed here (scope exit, not unwinding)
        TrackDestructor t4;
        ereport(LogLevel::kError, "multiple locals test");
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
    // t2, t3 destructed at scope exit (2 calls) + t1, t4 destructed
    // during unwinding (2 calls) = 4 total.
    EXPECT_EQ(g_destructor_calls, 4);
}

}  // namespace
