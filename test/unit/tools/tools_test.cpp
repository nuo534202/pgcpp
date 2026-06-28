// tools_test.cpp — Integration tests for the client tools module (M13).
//
// Tests three areas:
//   1. PsqlClient: connect to a running server, execute queries, verify results.
//   2. FormatQueryResult: table formatting of query results.
//   3. initdb tool: cluster initialization (via BootstrapCluster).
//
// The PsqlClient tests start a real server in a child process, connect via
// TCP, and verify end-to-end query execution through the full stack.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pgcpp/server/bootstrap.hpp"
#include "pgcpp/server/postmaster.hpp"
#include "pgcpp/tools/psql_client.hpp"

using mytoydb::server::BootstrapCluster;
using mytoydb::server::BootstrapResult;
using mytoydb::server::Postmaster;
using mytoydb::server::ServerConfig;
using mytoydb::tools::FormatQueryResult;
using mytoydb::tools::PsqlClient;
using mytoydb::tools::QueryResult;

namespace {

// ===========================================================================
// Helper functions (similar to server_test.cpp)
// ===========================================================================

void RunShell(const std::string& cmd) {
    int ret = system(cmd.c_str());
    (void)ret;
}

std::string MakeTempDir(const std::string& prefix) {
    return "/tmp/" + prefix + "_" + std::to_string(getpid());
}

bool IsDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Find a free TCP port by binding to port 0.
int FindFreePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 5433;

    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
        close(fd);
        return 5433;
    }

    socklen_t len = sizeof(sa);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&sa), &len) < 0) {
        close(fd);
        return 5433;
    }

    int port = ntohs(sa.sin_port);
    close(fd);
    return port;
}

// Start the postmaster in a child process.
pid_t StartServer(const std::string& data_dir, int port) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        // Child process: run the postmaster.
        ServerConfig config;
        config.data_dir = data_dir;
        config.port = port;
        config.listen_addr = "127.0.0.1";
        config.max_connections = 10;

        Postmaster postmaster(std::move(config));
        postmaster.Run();
        _exit(0);
    }

    return pid;
}

// Stop the server by sending SIGTERM and waiting.
void StopServer(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status;
        for (int i = 0; i < 50; ++i) {
            if (waitpid(pid, &status, WNOHANG) > 0)
                return;
            usleep(100000);  // 100ms
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

// Wait for the server to be ready by trying to connect.
bool WaitForServer(const std::string& host, int port, int timeout_secs = 5) {
    time_t deadline = time(nullptr) + timeout_secs;
    while (time(nullptr) < deadline) {
        PsqlClient client(host, port);
        if (client.Connect()) {
            client.Disconnect();
            return true;
        }
        usleep(100000);  // 100ms
    }
    return false;
}

}  // namespace

// ===========================================================================
// Part 1: FormatQueryResult tests (no server needed)
// ===========================================================================

TEST(FormatQueryResultTest, SelectResult) {
    QueryResult result;
    result.success = true;
    result.column_names = {"a", "b"};
    result.rows = {{"1", "hello"}, {"2", "world"}};
    result.command_tag = "SELECT 2";

    std::string formatted = FormatQueryResult(result);
    EXPECT_NE(formatted.find("a"), std::string::npos);
    EXPECT_NE(formatted.find("b"), std::string::npos);
    EXPECT_NE(formatted.find("1"), std::string::npos);
    EXPECT_NE(formatted.find("hello"), std::string::npos);
    EXPECT_NE(formatted.find("2"), std::string::npos);
    EXPECT_NE(formatted.find("world"), std::string::npos);
    EXPECT_NE(formatted.find("(2 rows)"), std::string::npos);
}

TEST(FormatQueryResultTest, EmptyResult) {
    QueryResult result;
    result.success = true;
    result.column_names = {"count"};
    result.rows = {};
    result.command_tag = "SELECT 0";

    std::string formatted = FormatQueryResult(result);
    EXPECT_NE(formatted.find("count"), std::string::npos);
    EXPECT_NE(formatted.find("(0 rows)"), std::string::npos);
}

TEST(FormatQueryResultTest, ErrorResult) {
    QueryResult result;
    result.success = false;
    result.error_message = "syntax error at or near \"SELCT\"";

    std::string formatted = FormatQueryResult(result);
    EXPECT_NE(formatted.find("ERROR"), std::string::npos);
    EXPECT_NE(formatted.find("syntax error"), std::string::npos);
}

TEST(FormatQueryResultTest, SingleRow) {
    QueryResult result;
    result.success = true;
    result.column_names = {"?column?"};
    result.rows = {{"42"}};
    result.command_tag = "SELECT 1";

    std::string formatted = FormatQueryResult(result);
    EXPECT_NE(formatted.find("42"), std::string::npos);
    EXPECT_NE(formatted.find("(1 row)"), std::string::npos);
}

TEST(FormatQueryResultTest, NonSelectResult) {
    QueryResult result;
    result.success = true;
    result.column_names = {};
    result.rows = {};
    result.command_tag = "INSERT 0 1";

    std::string formatted = FormatQueryResult(result);
    EXPECT_NE(formatted.find("INSERT 0 1"), std::string::npos);
}

// ===========================================================================
// Part 2: PsqlClient integration tests (requires a running server)
// ===========================================================================

class PsqlClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = MakeTempDir("mytoydb_psql_test");
        RunShell("rm -rf " + test_dir_);

        // Bootstrap the cluster.
        BootstrapResult result = BootstrapCluster(test_dir_);
        ASSERT_EQ(result, BootstrapResult::kOk) << "bootstrap failed";

        port_ = FindFreePort();
        server_pid_ = StartServer(test_dir_, port_);
        ASSERT_GT(server_pid_, 0) << "failed to start server";

        // Wait for the server to be ready.
        ASSERT_TRUE(WaitForServer("127.0.0.1", port_)) << "server did not become ready";
    }

    void TearDown() override {
        StopServer(server_pid_);
        RunShell("rm -rf " + test_dir_);
    }

    std::string test_dir_;
    int port_ = 0;
    pid_t server_pid_ = -1;
};

TEST_F(PsqlClientTest, ConnectAndDisconnect) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());
    EXPECT_TRUE(client.IsConnected());
    client.Disconnect();
    EXPECT_FALSE(client.IsConnected());
}

TEST_F(PsqlClientTest, Select1) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());

    QueryResult result = client.ExecuteQuery("SELECT 1");
    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
    ASSERT_EQ(result.rows[0].size(), 1u);
    EXPECT_EQ(result.rows[0][0], "1");
    EXPECT_EQ(result.command_tag, "SELECT 1");

    client.Disconnect();
}

TEST_F(PsqlClientTest, SelectArithmetic) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());

    QueryResult result = client.ExecuteQuery("SELECT 2 + 3");
    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], "5");

    client.Disconnect();
}

TEST_F(PsqlClientTest, SelectMultipleColumns) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());

    QueryResult result = client.ExecuteQuery("SELECT 1, 2, 3");
    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
    ASSERT_EQ(result.rows[0].size(), 3u);
    EXPECT_EQ(result.rows[0][0], "1");
    EXPECT_EQ(result.rows[0][1], "2");
    EXPECT_EQ(result.rows[0][2], "3");

    client.Disconnect();
}

TEST_F(PsqlClientTest, EmptyQuery) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());

    QueryResult result = client.ExecuteQuery(";");
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.column_names.empty());
    EXPECT_TRUE(result.rows.empty());

    client.Disconnect();
}

TEST_F(PsqlClientTest, SyntaxError) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());

    QueryResult result = client.ExecuteQuery("SELCT 1");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());

    client.Disconnect();
}

TEST_F(PsqlClientTest, MultipleQueries) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());

    // Execute multiple queries in sequence.
    QueryResult r1 = client.ExecuteQuery("SELECT 1");
    EXPECT_TRUE(r1.success);
    EXPECT_EQ(r1.rows[0][0], "1");

    QueryResult r2 = client.ExecuteQuery("SELECT 2 + 3");
    EXPECT_TRUE(r2.success);
    EXPECT_EQ(r2.rows[0][0], "5");

    QueryResult r3 = client.ExecuteQuery("SELECT 10 * 10");
    EXPECT_TRUE(r3.success);
    EXPECT_EQ(r3.rows[0][0], "100");

    client.Disconnect();
}

TEST_F(PsqlClientTest, TransactionControl) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());

    QueryResult begin = client.ExecuteQuery("BEGIN");
    EXPECT_TRUE(begin.success);

    QueryResult select = client.ExecuteQuery("SELECT 1");
    EXPECT_TRUE(select.success);
    EXPECT_EQ(select.rows[0][0], "1");

    QueryResult commit = client.ExecuteQuery("COMMIT");
    EXPECT_TRUE(commit.success);

    client.Disconnect();
}

TEST_F(PsqlClientTest, FormatResult) {
    PsqlClient client("127.0.0.1", port_);
    ASSERT_TRUE(client.Connect());

    QueryResult result = client.ExecuteQuery("SELECT 42");
    std::string formatted = FormatQueryResult(result);
    EXPECT_NE(formatted.find("42"), std::string::npos);
    EXPECT_NE(formatted.find("(1 row)"), std::string::npos);

    client.Disconnect();
}

// ===========================================================================
// Part 3: initdb tool tests (via BootstrapCluster)
// ===========================================================================

class InitdbTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = MakeTempDir("mytoydb_initdb_test");
        RunShell("rm -rf " + test_dir_);
    }

    void TearDown() override { RunShell("rm -rf " + test_dir_); }

    std::string test_dir_;
};

TEST_F(InitdbTest, CreatesCluster) {
    BootstrapResult result = BootstrapCluster(test_dir_);
    ASSERT_EQ(result, BootstrapResult::kOk);
    EXPECT_TRUE(IsDir(test_dir_));
    EXPECT_TRUE(IsDir(test_dir_ + "/base"));
    EXPECT_TRUE(IsDir(test_dir_ + "/global"));
    EXPECT_TRUE(IsDir(test_dir_ + "/pg_wal"));
    EXPECT_TRUE(IsFile(test_dir_ + "/mytoydb_version"));
}

TEST_F(InitdbTest, CanStartServerAfterInitdb) {
    // Bootstrap the cluster.
    ASSERT_EQ(BootstrapCluster(test_dir_), BootstrapResult::kOk);

    // Start the server.
    int port = FindFreePort();
    pid_t pid = StartServer(test_dir_, port);
    ASSERT_GT(pid, 0);

    // Wait for the server to be ready.
    ASSERT_TRUE(WaitForServer("127.0.0.1", port));

    // Connect and execute a query.
    PsqlClient client("127.0.0.1", port);
    ASSERT_TRUE(client.Connect());

    QueryResult result = client.ExecuteQuery("SELECT 1");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.rows[0][0], "1");

    client.Disconnect();
    StopServer(pid);
}

TEST_F(InitdbTest, FailsIfDirExists) {
    ASSERT_EQ(mkdir(test_dir_.c_str(), 0700), 0);
    BootstrapResult result = BootstrapCluster(test_dir_);
    EXPECT_EQ(result, BootstrapResult::kDirExists);
}
