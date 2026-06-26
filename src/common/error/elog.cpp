#include "mytoydb/common/error/elog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"

namespace mytoydb::error {

namespace {

// Maximum depth of nested PG_TRY blocks.
constexpr int kMaxExceptionStackDepth = 64;

// Thread-local error state.
thread_local ErrorData last_error_data;
thread_local JmpBufEntry* exception_stack[kMaxExceptionStackDepth];
thread_local int exception_stack_top = 0;

// ErrorContext: a memory context that survives error longjmp so that
// ErrorData strings allocated within it are not freed prematurely.
// In PostgreSQL this is a dedicated AllocSetContext named "ErrorContext".
memory::MemoryContext* error_context = nullptr;

// Convert LogLevel to a printable string.
const char* LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug5:
            return "DEBUG5";
        case LogLevel::kDebug4:
            return "DEBUG4";
        case LogLevel::kDebug3:
            return "DEBUG3";
        case LogLevel::kDebug2:
            return "DEBUG2";
        case LogLevel::kDebug1:
            return "DEBUG1";
        case LogLevel::kLog:
            return "LOG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kNotice:
            return "NOTICE";
        case LogLevel::kWarning:
            return "WARNING";
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kFatal:
            return "FATAL";
        case LogLevel::kPanic:
            return "PANIC";
    }
    return "UNKNOWN";
}

}  // namespace

ErrorData* GetErrorData() {
    return &last_error_data;
}

void InitErrorSubsystem() {
    if (error_context != nullptr) {
        return;
    }
    error_context = memory::AllocSetContext::Create("ErrorContext");
}

JmpBufEntry* PushExceptionStack() {
    if (exception_stack_top >= kMaxExceptionStackDepth) {
        std::fprintf(stderr, "exception stack overflow\n");
        std::abort();
    }
    auto* entry = new JmpBufEntry();
    exception_stack[exception_stack_top] = entry;
    ++exception_stack_top;
    return entry;
}

void PopExceptionStack(JmpBufEntry* entry) {
    if (exception_stack_top <= 0) {
        std::fprintf(stderr, "exception stack underflow\n");
        return;
    }
    --exception_stack_top;
    if (exception_stack[exception_stack_top] == entry) {
        delete exception_stack[exception_stack_top];
        exception_stack[exception_stack_top] = nullptr;
    } else {
        // Mismatched push/pop — restore top and warn.
        ++exception_stack_top;
        std::fprintf(stderr, "warning: exception stack pop mismatch\n");
    }
}

void EreportImpl(LogLevel elevel, const char* filename, const char* funcname, int lineno,
                 std::string message) {
    // Fill in the error data.
    last_error_data.elevel = elevel;
    last_error_data.filename = filename;
    last_error_data.funcname = funcname;
    last_error_data.lineno = lineno;
    // Copy (not move) the message into thread-local storage.
    // Move-assignment is avoided because libstdc++ implements it via swap,
    // which would leave the old last_error_data.message buffer in `message`
    // (the parameter) — and `message` would leak when longjmp bypasses its
    // destructor. assign() properly frees the old buffer (or reuses it).
    last_error_data.message.assign(message);

    if (static_cast<int>(elevel) < static_cast<int>(LogLevel::kError)) {
        // Non-error level: print to stderr and return normally.
        // message's destructor will run on return, freeing its buffer.
        std::fprintf(stderr, "[%s] %s (%s:%d:%s)\n", LogLevelToString(elevel),
                     last_error_data.message.c_str(), filename, lineno, funcname);
        return;
    }

    // ERROR/FATAL/PANIC: print, free the parameter's buffer, then longjmp.
    std::fprintf(stderr, "[%s] %s (%s:%d:%s)\n", LogLevelToString(elevel),
                 last_error_data.message.c_str(), filename, lineno, funcname);

    // longjmp bypasses C++ destructors, so `message`'s heap buffer would
    // leak. Call the destructor explicitly to free it. This is safe because
    // longjmp will skip the automatic destructor call (no double-free).
    message.~basic_string();

    if (exception_stack_top > 0) {
        JmpBufEntry* top = exception_stack[exception_stack_top - 1];
        std::longjmp(top->buf, 1);
    }

    // No handler on the stack — abort the process.
    std::fprintf(stderr, "No error handler registered; aborting.\n");
    std::abort();
}

}  // namespace mytoydb::error
