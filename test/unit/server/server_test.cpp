// server_test.cpp — Integration tests for the Postmaster/Bootstrap module (M12).
//
// Tests three areas:
//   1. Bootstrap: directory creation, marker file, IsBootstrapped.
//   2. Postmaster: server startup, TCP connection, simple query protocol,
//      graceful shutdown via SIGTERM.
//   3. Main entry point: argument parsing for server/bootstrap/help modes.
//
// The postmaster tests fork a child process to run the server, then connect
// via TCP from the test process to verify end-to-end protocol behavior.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pgcpp/server/bootstrap.hpp"
#include "pgcpp/server/main.hpp"
#include "pgcpp/server/postmaster.hpp"

using mytoydb::server::BootstrapCluster;
using mytoydb::server::BootstrapResult;
using mytoydb::server::IsBootstrapped;
using mytoydb::server::Postmaster;
using mytoydb::server::ServerConfig;
using mytoydb::server::ServerMode;
using mytoydb::server::ServerOptions;

namespace {

// ===========================================================================
// Helper functions
// ===========================================================================

// Run a shell command (used for directory cleanup).
void RunShell(const std::string& cmd) {
    int ret = system(cmd.c_str());
    (void)ret;
}

// Create a unique temp directory path.
std::string MakeTempDir(const std::string& prefix) {
    return "/tmp/" + prefix + "_" + std::to_string(getpid());
}

// Check if a path is a directory.
bool IsDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Check if a path is a regular file.
bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Read a file's contents.
std::string ReadFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr)
        return "";
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return std::string(buf, n);
}

// Write exactly len bytes to a fd (handles partial writes).
bool WriteAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

// Read exactly len bytes from a fd (handles partial reads).
bool ReadAll(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

// Connect to a TCP server with retries.
// Returns the fd on success, -1 on failure.
int ConnectToServer(const std::string& addr, int port, int timeout_secs = 5) {
    time_t deadline = time(nullptr) + timeout_secs;
    while (time(nullptr) < deadline) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            usleep(100000);
            continue;
        }

        struct sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) <= 0) {
            close(fd);
            return -1;
        }

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0) {
            // Set a read timeout.
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return fd;
        }

        close(fd);
        usleep(100000);  // 100ms
    }
    return -1;
}

// Send the PostgreSQL startup message.
bool SendStartupMessage(int fd) {
    // Startup message: length (4) + protocol version (4) + "user\0mytoydb\0\0"
    std::string payload;
    // Protocol version 3.0
    int32_t proto = htonl(0x00030000);
    payload.append(reinterpret_cast<const char*>(&proto), 4);
    // User parameter
    payload.append("user\0mytoydb\0", 12);
    // Terminator
    payload.push_back('\0');

    int32_t len = htonl(static_cast<int32_t>(4 + payload.size()));
    std::string msg(reinterpret_cast<const char*>(&len), 4);
    msg += payload;
    return WriteAll(fd, msg.data(), msg.size());
}

// Read a single protocol message from the server.
// Sets type and payload. Returns true on success.
bool ReadServerMessage(int fd, char* type, std::string* payload) {
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

    size_t payload_len = static_cast<size_t>(length) - 4;
    if (payload_len > 0) {
        payload->resize(payload_len);
        if (!ReadAll(fd, payload->data(), payload_len))
            return false;
    } else {
        payload->clear();
    }
    return true;
}

// Read and discard the startup sequence (AuthenticationOk, ParameterStatus*,
// BackendKeyData, ReadyForQuery). Returns true if ReadyForQuery was received.
bool ReadStartupResponse(int fd) {
    bool got_ready = false;
    while (true) {
        char type;
        std::string payload;
        if (!ReadServerMessage(fd, &type, &payload))
            return false;

        switch (type) {
            case 'R':  // AuthenticationOk
                break;
            case 'S':  // ParameterStatus
                break;
            case 'K':  // BackendKeyData
                break;
            case 'Z':  // ReadyForQuery
                got_ready = true;
                return got_ready;
            default:
                // Unexpected message
                return false;
        }
    }
}

// Send a simple query message ('Q').
bool SendSimpleQuery(int fd, const std::string& query) {
    std::string payload = query;
    payload.push_back('\0');  // null-terminated

    std::string msg;
    msg.push_back('Q');
    int32_t len = htonl(static_cast<int32_t>(4 + payload.size()));
    msg.append(reinterpret_cast<const char*>(&len), 4);
    msg += payload;
    return WriteAll(fd, msg.data(), msg.size());
}

// Send a Terminate message ('X') and close the connection.
void SendTerminate(int fd) {
    std::string msg;
    msg.push_back('X');
    int32_t len = htonl(4);
    msg.append(reinterpret_cast<const char*>(&len), 4);
    WriteAll(fd, msg.data(), msg.size());
    close(fd);
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
// Returns the child PID (>0) on success, -1 on failure.
// The child will listen on the given port and data directory.
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
        // Wait with a timeout.
        for (int i = 0; i < 50; ++i) {
            if (waitpid(pid, &status, WNOHANG) > 0)
                return;
            usleep(100000);  // 100ms
        }
        // Force kill if still running.
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

}  // namespace

// ===========================================================================
// Part 1: Bootstrap tests
// ===========================================================================

class BootstrapTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = MakeTempDir("mytoydb_bootstrap_test");
        RunShell("rm -rf " + test_dir_);
    }

    void TearDown() override { RunShell("rm -rf " + test_dir_); }

    std::string test_dir_;
};

TEST_F(BootstrapTest, CreatesDirectory) {
    BootstrapResult result = BootstrapCluster(test_dir_);
    ASSERT_EQ(result, BootstrapResult::kOk);
    EXPECT_TRUE(IsDir(test_dir_));
}

TEST_F(BootstrapTest, CreatesSubdirectories) {
    BootstrapResult result = BootstrapCluster(test_dir_);
    ASSERT_EQ(result, BootstrapResult::kOk);
    EXPECT_TRUE(IsDir(test_dir_ + "/base"));
    EXPECT_TRUE(IsDir(test_dir_ + "/global"));
    EXPECT_TRUE(IsDir(test_dir_ + "/pg_wal"));
}

TEST_F(BootstrapTest, WritesMarker) {
    BootstrapResult result = BootstrapCluster(test_dir_);
    ASSERT_EQ(result, BootstrapResult::kOk);
    EXPECT_TRUE(IsFile(test_dir_ + "/mytoydb_version"));
    std::string version = ReadFile(test_dir_ + "/mytoydb_version");
    EXPECT_FALSE(version.empty());
    EXPECT_EQ(version, "mytoydb-1.0");
}

TEST_F(BootstrapTest, IsBootstrappedReturnsTrue) {
    EXPECT_FALSE(IsBootstrapped(test_dir_));
    BootstrapResult result = BootstrapCluster(test_dir_);
    ASSERT_EQ(result, BootstrapResult::kOk);
    EXPECT_TRUE(IsBootstrapped(test_dir_));
}

TEST_F(BootstrapTest, FailsIfDirExists) {
    // Create the directory first.
    ASSERT_EQ(mkdir(test_dir_.c_str(), 0700), 0);
    BootstrapResult result = BootstrapCluster(test_dir_);
    EXPECT_EQ(result, BootstrapResult::kDirExists);
}

TEST_F(BootstrapTest, ResultToString) {
    EXPECT_STREQ(BootstrapResultToString(BootstrapResult::kOk), "success");
    EXPECT_STREQ(BootstrapResultToString(BootstrapResult::kDirExists),
                 "data directory already exists");
    EXPECT_STREQ(BootstrapResultToString(BootstrapResult::kMkdirFailed),
                 "failed to create directory");
    EXPECT_STREQ(BootstrapResultToString(BootstrapResult::kInitFailed), "initialization failed");
}

// ===========================================================================
// Part 2: Postmaster integration tests
// ===========================================================================

class PostmasterTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = MakeTempDir("mytoydb_server_test");
        RunShell("rm -rf " + test_dir_);

        // Bootstrap the cluster first.
        BootstrapResult result = BootstrapCluster(test_dir_);
        ASSERT_EQ(result, BootstrapResult::kOk)
            << "bootstrap failed: " << BootstrapResultToString(result);

        port_ = FindFreePort();
        server_pid_ = -1;
    }

    void TearDown() override {
        StopServer(server_pid_);
        RunShell("rm -rf " + test_dir_);
    }

    void StartServerAndConnect() {
        server_pid_ = StartServer(test_dir_, port_);
        ASSERT_GT(server_pid_, 0) << "failed to fork server";

        int fd = ConnectToServer("127.0.0.1", port_);
        ASSERT_GE(fd, 0) << "failed to connect to server on port " << port_;

        ASSERT_TRUE(SendStartupMessage(fd)) << "failed to send startup message";
        ASSERT_TRUE(ReadStartupResponse(fd)) << "failed to read startup response";

        client_fd_ = fd;
    }

    void Disconnect() {
        if (client_fd_ >= 0) {
            SendTerminate(client_fd_);
            client_fd_ = -1;
        }
    }

    std::string test_dir_;
    int port_ = 0;
    pid_t server_pid_ = -1;
    int client_fd_ = -1;
};

TEST_F(PostmasterTest, ServerStartsAndAcceptsConnections) {
    StartServerAndConnect();
    // If we got here, the server started and accepted our connection.
    SUCCEED();
    Disconnect();
}

TEST_F(PostmasterTest, SimpleQuerySelect1) {
    StartServerAndConnect();

    ASSERT_TRUE(SendSimpleQuery(client_fd_, "SELECT 1"));

    // Read the response messages.
    // Expected: RowDescription ('T'), DataRow ('D'), CommandComplete ('C'),
    //           ReadyForQuery ('Z')
    bool got_row_desc = false;
    bool got_data_row = false;
    bool got_cmd_complete = false;
    bool got_ready = false;

    for (int i = 0; i < 10 && !got_ready; ++i) {
        char type;
        std::string payload;
        if (!ReadServerMessage(client_fd_, &type, &payload)) {
            FAIL() << "failed to read message";
            break;
        }

        switch (type) {
            case 'T':  // RowDescription
                got_row_desc = true;
                break;
            case 'D':  // DataRow
                got_data_row = true;
                break;
            case 'C':  // CommandComplete
                got_cmd_complete = true;
                break;
            case 'Z':  // ReadyForQuery
                got_ready = true;
                break;
            case 'E': {  // ErrorResponse
                // Read the error message from the payload.
                std::string msg;
                for (size_t j = 0; j < payload.size();) {
                    char field = payload[j];
                    if (field == '\0')
                        break;
                    j++;
                    size_t end = payload.find('\0', j);
                    if (end == std::string::npos)
                        break;
                    if (field == 'M') {
                        msg = payload.substr(j, end - j);
                    }
                    j = end + 1;
                }
                FAIL() << "server returned error: " << msg;
                break;
            }
            default:
                break;
        }
    }

    EXPECT_TRUE(got_row_desc);
    EXPECT_TRUE(got_data_row);
    EXPECT_TRUE(got_cmd_complete);
    EXPECT_TRUE(got_ready);

    Disconnect();
}

TEST_F(PostmasterTest, SimpleQuerySelectArithmetic) {
    StartServerAndConnect();

    ASSERT_TRUE(SendSimpleQuery(client_fd_, "SELECT 2 + 3"));

    bool got_data_row = false;
    bool got_ready = false;

    for (int i = 0; i < 10 && !got_ready; ++i) {
        char type;
        std::string payload;
        if (!ReadServerMessage(client_fd_, &type, &payload)) {
            FAIL() << "failed to read message";
            break;
        }

        if (type == 'D') {
            // DataRow: int16 column_count, then per column: int32 length, then bytes.
            got_data_row = true;
            if (payload.size() >= 6) {
                int16_t ncols = static_cast<int16_t>((static_cast<uint8_t>(payload[0]) << 8) |
                                                     static_cast<uint8_t>(payload[1]));
                EXPECT_EQ(ncols, 1);
                int32_t col_len = static_cast<int32_t>((static_cast<uint8_t>(payload[2]) << 24) |
                                                       (static_cast<uint8_t>(payload[3]) << 16) |
                                                       (static_cast<uint8_t>(payload[4]) << 8) |
                                                       static_cast<uint8_t>(payload[5]));
                EXPECT_GT(col_len, 0);
                if (col_len > 0 && static_cast<size_t>(col_len) + 6 <= payload.size()) {
                    std::string value = payload.substr(6, static_cast<size_t>(col_len));
                    EXPECT_EQ(value, "5");
                }
            }
        } else if (type == 'Z') {
            got_ready = true;
        } else if (type == 'E') {
            std::string msg;
            for (size_t j = 0; j < payload.size();) {
                char field = payload[j];
                if (field == '\0')
                    break;
                j++;
                size_t end = payload.find('\0', j);
                if (end == std::string::npos)
                    break;
                if (field == 'M')
                    msg = payload.substr(j, end - j);
                j = end + 1;
            }
            FAIL() << "server returned error: " << msg;
        }
    }

    EXPECT_TRUE(got_data_row);
    EXPECT_TRUE(got_ready);

    Disconnect();
}

TEST_F(PostmasterTest, EmptyQuery) {
    StartServerAndConnect();

    ASSERT_TRUE(SendSimpleQuery(client_fd_, ";"));

    bool got_empty_response = false;
    bool got_ready = false;

    for (int i = 0; i < 10 && !got_ready; ++i) {
        char type;
        std::string payload;
        if (!ReadServerMessage(client_fd_, &type, &payload)) {
            FAIL() << "failed to read message";
            break;
        }

        if (type == 'I') {
            got_empty_response = true;
        } else if (type == 'Z') {
            got_ready = true;
        } else if (type == 'E') {
            std::string msg;
            for (size_t j = 0; j < payload.size();) {
                char field = payload[j];
                if (field == '\0')
                    break;
                j++;
                size_t end = payload.find('\0', j);
                if (end == std::string::npos)
                    break;
                if (field == 'M')
                    msg = payload.substr(j, end - j);
                j = end + 1;
            }
            FAIL() << "server returned error: " << msg;
        }
    }

    EXPECT_TRUE(got_empty_response);
    EXPECT_TRUE(got_ready);

    Disconnect();
}

TEST_F(PostmasterTest, ServerShutdownOnSIGTERM) {
    StartServerAndConnect();
    Disconnect();

    // Send SIGTERM to the server.
    ASSERT_GT(server_pid_, 0);
    kill(server_pid_, SIGTERM);

    // Wait for the server to exit.
    int status;
    bool exited = false;
    for (int i = 0; i < 50; ++i) {
        if (waitpid(server_pid_, &status, WNOHANG) > 0) {
            exited = true;
            break;
        }
        usleep(100000);  // 100ms
    }

    EXPECT_TRUE(exited) << "server did not exit after SIGTERM";
    server_pid_ = -1;  // Don't try to stop it again in TearDown.
}

// ===========================================================================
// Part 3: Main entry point tests
// ===========================================================================

TEST(MainTest, ParseArgsBootstrap) {
    ServerOptions opts;
    char arg0[] = "mytoydb";
    char arg1[] = "--bootstrap";
    char arg2[] = "-D";
    char arg3[] = "/tmp/test_dir";
    char* argv[] = {arg0, arg1, arg2, arg3, nullptr};

    EXPECT_TRUE(mytoydb::server::ParseArgs(4, argv, &opts));
    EXPECT_EQ(opts.mode, ServerMode::kBootstrap);
    EXPECT_EQ(opts.data_dir, "/tmp/test_dir");
}

TEST(MainTest, ParseArgsServer) {
    ServerOptions opts;
    char arg0[] = "mytoydb";
    char arg1[] = "-D";
    char arg2[] = "/tmp/test_dir";
    char arg3[] = "-p";
    char arg4[] = "5433";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, nullptr};

    EXPECT_TRUE(mytoydb::server::ParseArgs(5, argv, &opts));
    EXPECT_EQ(opts.mode, ServerMode::kServer);
    EXPECT_EQ(opts.data_dir, "/tmp/test_dir");
    EXPECT_EQ(opts.port, 5433);
}

TEST(MainTest, ParseArgsHelp) {
    ServerOptions opts;
    char arg0[] = "mytoydb";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1, nullptr};

    EXPECT_TRUE(mytoydb::server::ParseArgs(2, argv, &opts));
    EXPECT_EQ(opts.mode, ServerMode::kHelp);
}

TEST(MainTest, ParseArgsFailsWithoutDataDir) {
    ServerOptions opts;
    char arg0[] = "mytoydb";
    char* argv[] = {arg0, nullptr};

    EXPECT_FALSE(mytoydb::server::ParseArgs(1, argv, &opts));
}

TEST(MainTest, ParseArgsFailsWithUnknownOption) {
    ServerOptions opts;
    char arg0[] = "mytoydb";
    char arg1[] = "--unknown";
    char arg2[] = "-D";
    char arg3[] = "/tmp/test_dir";
    char* argv[] = {arg0, arg1, arg2, arg3, nullptr};

    EXPECT_FALSE(mytoydb::server::ParseArgs(4, argv, &opts));
}

TEST(MainTest, ParseArgsMaxConnections) {
    ServerOptions opts;
    char arg0[] = "mytoydb";
    char arg1[] = "-D";
    char arg2[] = "/tmp/test_dir";
    char arg3[] = "-N";
    char arg4[] = "50";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, nullptr};

    EXPECT_TRUE(mytoydb::server::ParseArgs(5, argv, &opts));
    EXPECT_EQ(opts.max_connections, 50);
}

TEST(MainTest, ParseArgsListenAddr) {
    ServerOptions opts;
    char arg0[] = "mytoydb";
    char arg1[] = "-D";
    char arg2[] = "/tmp/test_dir";
    char arg3[] = "-h";
    char arg4[] = "0.0.0.0";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, nullptr};

    EXPECT_TRUE(mytoydb::server::ParseArgs(5, argv, &opts));
    EXPECT_EQ(opts.listen_addr, "0.0.0.0");
}
