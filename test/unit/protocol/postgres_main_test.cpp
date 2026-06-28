// postgres_main_test.cpp — Unit tests for PostgresMain (M11 Phase 15.9).
//
// Tests the PostgresMain main loop, ProcessInterrupts, and SetInterruptPending.
// The interrupt tests are pure unit tests (no sockets). The PostgresMain
// integration test uses a socketpair + fork to exercise the full loop.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/bootstrap_catalog.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/syscache.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/protocol/postgres.hpp"
#include "pgcpp/protocol/pqformat.hpp"
#include "pgcpp/server/postmaster.hpp"
#include "pgcpp/storage/bufmgr.hpp"
#include "pgcpp/storage/smgr.hpp"
#include "pgcpp/transaction/snapshot.hpp"
#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xact.hpp"

using pgcpp::access::InitializeRelcache;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::protocol::PostgresMain;
using pgcpp::protocol::ProcessInterrupts;
using pgcpp::protocol::SetInterruptPending;
using pgcpp::server::SocketSink;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;

namespace {

class PostgresMainTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("pgmain_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_pgmain_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
    }

    void TearDown() override {
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
        InitializeSnapshotManager();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

    // Write a wire-format message to fd.
    static void WriteMessage(int fd, char type, const std::string& payload) {
        std::string msg;
        msg.push_back(type);
        int32_t len = htonl(static_cast<int32_t>(4 + payload.size()));
        msg.append(reinterpret_cast<const char*>(&len), 4);
        msg += payload;
        std::size_t written = 0;
        while (written < msg.size()) {
            ssize_t n = write(fd, msg.data() + written, msg.size() - written);
            if (n <= 0)
                break;
            written += static_cast<std::size_t>(n);
        }
    }

    // Read a wire-format message from fd.
    static bool ReadMessage(int fd, char* type, std::string& payload) {
        if (!ReadAll(fd, type, 1))
            return false;
        char len_buf[4];
        if (!ReadAll(fd, len_buf, 4))
            return false;
        int32_t length = static_cast<int32_t>(
            (static_cast<uint8_t>(len_buf[0]) << 24) | (static_cast<uint8_t>(len_buf[1]) << 16) |
            (static_cast<uint8_t>(len_buf[2]) << 8) | static_cast<uint8_t>(len_buf[3]));
        if (length < 4)
            return false;
        std::size_t payload_len = static_cast<std::size_t>(length) - 4;
        if (payload_len > 0) {
            payload.resize(payload_len);
            if (!ReadAll(fd, payload.data(), payload_len))
                return false;
        } else {
            payload.clear();
        }
        return true;
    }

    static bool ReadAll(int fd, char* buf, std::size_t len) {
        std::size_t got = 0;
        while (got < len) {
            ssize_t n = read(fd, buf + got, len - got);
            if (n <= 0)
                return false;
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// --- ProcessInterrupts / SetInterruptPending ---

TEST_F(PostgresMainTest, ProcessInterrupts_NoopWhenNotPending) {
    // Without SetInterruptPending, ProcessInterrupts should be a no-op.
    // Wrap in PG_TRY to catch any unexpected ereport.
    bool caught = false;
    PG_TRY() {
        ProcessInterrupts();
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    EXPECT_FALSE(caught);
}

TEST_F(PostgresMainTest, SetInterruptPending_TriggersProcessInterrupts) {
    SetInterruptPending();
    bool caught = false;
    std::string message;
    PG_TRY() {
        ProcessInterrupts();
    }
    PG_CATCH() {
        caught = true;
        pgcpp::error::ErrorData* ed = pgcpp::error::GetErrorData();
        message = ed ? ed->message : "";
    }
    PG_END_TRY();
    EXPECT_TRUE(caught);
    EXPECT_NE(message.find("canceling"), std::string::npos);
}

TEST_F(PostgresMainTest, ProcessInterrupts_ClearsFlag) {
    SetInterruptPending();
    bool first_caught = false;
    PG_TRY() {
        ProcessInterrupts();
    }
    PG_CATCH() {
        first_caught = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(first_caught);

    // Second call should be a no-op (flag was cleared).
    bool second_caught = false;
    PG_TRY() {
        ProcessInterrupts();
    }
    PG_CATCH() {
        second_caught = true;
    }
    PG_END_TRY();
    EXPECT_FALSE(second_caught);
}

// --- PostgresMain integration test (fork + socketpair) ---

TEST_F(PostgresMainTest, PostgresMain_TerminateReturnsImmediately) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child: run PostgresMain.
        close(fds[0]);
        SocketSink sink(fds[1]);
        PostgresMain(fds[1], &sink);
        // Skip TearDown (the parent owns the global state).
        _exit(0);
    }

    // Parent: send 'X' (Terminate) and wait for child to exit.
    close(fds[1]);
    WriteMessage(fds[0], 'X', "");

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    close(fds[0]);
}

TEST_F(PostgresMainTest, PostgresMain_SimpleQuerySelect) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child: run PostgresMain.
        close(fds[0]);
        SocketSink sink(fds[1]);
        PostgresMain(fds[1], &sink);
        _exit(0);
    }

    // Parent: send a simple query 'Q' message.
    close(fds[1]);

    std::string query = "SELECT 42;";
    std::string query_payload = query;
    query_payload.push_back('\0');  // NUL-terminated string
    WriteMessage(fds[0], 'Q', query_payload);

    // Read response messages until ReadyForQuery.
    char type = 0;
    std::string payload;
    bool got_command_complete = false;
    bool got_ready_for_query = false;
    int msg_count = 0;
    while (msg_count < 10 && ReadMessage(fds[0], &type, payload)) {
        ++msg_count;
        if (type == 'C') {
            got_command_complete = true;
        } else if (type == 'Z') {
            got_ready_for_query = true;
            break;
        }
    }

    EXPECT_TRUE(got_command_complete);
    EXPECT_TRUE(got_ready_for_query);

    // Send Terminate to end the child.
    WriteMessage(fds[0], 'X', "");
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    close(fds[0]);
}

TEST_F(PostgresMainTest, PostgresMain_ClientDisconnectEndsLoop) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child: run PostgresMain.
        close(fds[0]);
        SocketSink sink(fds[1]);
        PostgresMain(fds[1], &sink);
        _exit(0);
    }

    // Parent: close the socket immediately (simulates client disconnect).
    close(fds[1]);
    close(fds[0]);

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
}

// --- 'Z' transaction status byte (Task 15.10.3) ---
//
// The ReadyForQuery message carries a single-byte transaction status:
//   'I' = idle, 'T' = in transaction, 'E' = in failed transaction.
// The fixture's SetUp calls BeginTransactionBlock(), so the forked child
// inherits a transaction block and the status should reflect that.

TEST_F(PostgresMainTest, ReadyForQueryStatusIsInTransactionAfterSuccess) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(fds[0]);
        SocketSink sink(fds[1]);
        PostgresMain(fds[1], &sink);
        _exit(0);
    }
    close(fds[1]);

    // Send a successful simple query.
    std::string query = "SELECT 1;";
    query.push_back('\0');
    WriteMessage(fds[0], 'Q', query);

    // Read until ReadyForQuery and check the status byte.
    char type = 0;
    std::string payload;
    bool got_ready = false;
    char z_status = 0;
    for (int i = 0; i < 10 && ReadMessage(fds[0], &type, payload); ++i) {
        if (type == 'Z') {
            got_ready = true;
            ASSERT_FALSE(payload.empty());
            z_status = payload[0];
            break;
        }
    }
    EXPECT_TRUE(got_ready);
    // We're inside a transaction block (from the fixture), so status is 'T'.
    EXPECT_EQ(z_status, 'T');

    WriteMessage(fds[0], 'X', "");
    int status = 0;
    waitpid(pid, &status, 0);
    close(fds[0]);
}

TEST_F(PostgresMainTest, ReadyForQueryStatusIsInFailedTransactionAfterError) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(fds[0]);
        SocketSink sink(fds[1]);
        PostgresMain(fds[1], &sink);
        _exit(0);
    }
    close(fds[1]);

    // Send a query that will fail (nonexistent table).
    std::string query = "SELECT * FROM nonexistent_table_xyz;";
    query.push_back('\0');
    WriteMessage(fds[0], 'Q', query);

    // Read until ReadyForQuery, capturing ErrorResponse + status byte.
    char type = 0;
    std::string payload;
    bool got_error = false;
    bool got_ready = false;
    char z_status = 0;
    for (int i = 0; i < 10 && ReadMessage(fds[0], &type, payload); ++i) {
        if (type == 'E') {
            got_error = true;  // ErrorResponse (server -> client).
        } else if (type == 'Z') {
            got_ready = true;
            ASSERT_FALSE(payload.empty());
            z_status = payload[0];
            break;
        }
    }
    EXPECT_TRUE(got_error) << "expected an ErrorResponse before ReadyForQuery";
    EXPECT_TRUE(got_ready);
    // We're inside a transaction block (from the fixture) and the query failed,
    // so the status must be 'E' (in failed transaction).
    EXPECT_EQ(z_status, 'E');

    WriteMessage(fds[0], 'X', "");
    int status = 0;
    waitpid(pid, &status, 0);
    close(fds[0]);
}

}  // namespace
