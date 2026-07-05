// postmaster_startup_test.cpp — Unit tests for ProcessStartupPacket (M12 Phase 15.10.2).
//
// Tests parsing of the PostgreSQL startup packet sequence:
//   - v3.0 startup with user/database/application_name
//   - SSLRequest preamble (server replies 'N', then real startup follows)
//   - GSSENCRequest preamble
//   - CancelRequest (returns invalid)
//   - database defaults to user when not supplied
//
// Uses socketpair + fork to exercise the real fd-based read path.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "server/postmaster.hpp"

using pgcpp::server::ProcessStartupPacket;
using pgcpp::server::StartupPacketResult;

namespace {

// Write all bytes (handles partial writes).
bool WriteAll(int fd, const void* data, std::size_t len) {
    const char* p = static_cast<const char*>(data);
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

// Read all bytes (handles partial reads).
bool ReadAll(int fd, void* buf, std::size_t len) {
    char* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

// Read a single byte (used for the SSL/GSS 'N' reply).
bool ReadByte(int fd, char* b) {
    return ReadAll(fd, b, 1);
}

// Build a v3.0 startup packet body (without the 4-byte length prefix).
// `pairs` is a sequence of null-terminated key/value strings appended in order.
std::string BuildStartupBody(uint32_t proto_version,
                             const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::string body;
    uint32_t net_proto = htonl(proto_version);
    body.append(reinterpret_cast<const char*>(&net_proto), 4);
    for (const auto& [k, v] : pairs) {
        body += k;
        body.push_back('\0');
        body += v;
        body.push_back('\0');
    }
    body.push_back('\0');  // terminator
    return body;
}

// Write a length-prefixed startup packet (no type byte).
void WriteStartupPacket(int fd, uint32_t proto_version,
                        const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::string body = BuildStartupBody(proto_version, pairs);
    uint32_t len = htonl(static_cast<uint32_t>(4 + body.size()));
    WriteAll(fd, &len, 4);
    WriteAll(fd, body.data(), body.size());
}

// Write a special-code startup packet (SSL/GSS/Cancel) — just length + code.
void WriteSpecialPacket(int fd, uint32_t code) {
    uint32_t len = htonl(8);
    uint32_t net_code = htonl(code);
    WriteAll(fd, &len, 4);
    WriteAll(fd, &net_code, 4);
}

// Write a CancelRequest packet (length 16: len + code + pid + secret).
void WriteCancelPacket(int fd, uint32_t pid, uint32_t secret) {
    uint32_t len = htonl(16);
    uint32_t net_code = htonl(pgcpp::server::kCancelRequestCode);
    uint32_t net_pid = htonl(pid);
    uint32_t net_secret = htonl(secret);
    WriteAll(fd, &len, 4);
    WriteAll(fd, &net_code, 4);
    WriteAll(fd, &net_pid, 4);
    WriteAll(fd, &net_secret, 4);
}

// Run ProcessStartupPacket in a child process over a socketpair.
// `writer` is called by the parent to send packets to the child.
// `child_result` is filled in by the child via a pipe.
struct ChildOutcome {
    bool exited = false;
    int status = 0;
    StartupPacketResult result;
};

ChildOutcome RunChildAndSend(const std::function<void(int fd)>& writer) {
    int sock[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sock) != 0) {
        return {};
    }
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        close(sock[0]);
        close(sock[1]);
        return {};
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(sock[0]);
        close(sock[1]);
        close(pipefd[0]);
        close(pipefd[1]);
        return {};
    }

    if (pid == 0) {
        // Child: close parent ends, run ProcessStartupPacket, write result.
        close(sock[1]);
        close(pipefd[0]);
        StartupPacketResult r = ProcessStartupPacket(sock[0]);
        // Serialize: write valid byte, then user/database/app_name with NUL separators.
        std::string out;
        out.push_back(r.valid ? 1 : 0);
        out += r.user;
        out.push_back('\0');
        out += r.database;
        out.push_back('\0');
        out += r.application_name;
        out.push_back('\0');
        uint32_t net_proto = htonl(r.protocol_version);
        out.append(reinterpret_cast<const char*>(&net_proto), 4);
        ssize_t wr = write(pipefd[1], out.data(), out.size());
        (void)wr;
        close(pipefd[1]);
        close(sock[0]);
        _exit(0);
    }

    // Parent: write packets, then read result, then wait.
    close(sock[0]);
    close(pipefd[1]);
    writer(sock[1]);
    shutdown(sock[1], SHUT_WR);

    ChildOutcome o;
    std::string buf;
    char tmp[256];
    while (true) {
        ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
        if (n <= 0)
            break;
        buf.append(tmp, static_cast<std::size_t>(n));
    }
    close(pipefd[0]);
    close(sock[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    o.exited = WIFEXITED(status);
    o.status = WEXITSTATUS(status);

    // Deserialize.
    if (buf.size() >= 5) {
        o.result.valid = (buf[0] == 1);
        std::size_t pos = 1;
        auto read_str = [&]() {
            std::string s;
            while (pos < buf.size() && buf[pos] != '\0') {
                s.push_back(buf[pos++]);
            }
            if (pos < buf.size())
                ++pos;
            return s;
        };
        o.result.user = read_str();
        o.result.database = read_str();
        o.result.application_name = read_str();
        if (pos + 4 <= buf.size()) {
            uint32_t net_proto = 0;
            std::memcpy(&net_proto, buf.data() + pos, 4);
            o.result.protocol_version = ntohl(net_proto);
        }
    }
    return o;
}

}  // namespace

// --- v3.0 startup parsing ---

TEST(ProcessStartupPacketTest, ParsesV3StartupWithUser) {
    auto o = RunChildAndSend([](int fd) {
        WriteStartupPacket(fd, 0x00030000, {{"user", "alice"}});
    });
    ASSERT_TRUE(o.exited);
    EXPECT_EQ(o.status, 0);
    EXPECT_TRUE(o.result.valid);
    EXPECT_EQ(o.result.user, "alice");
    EXPECT_EQ(o.result.protocol_version, 0x00030000u);
}

TEST(ProcessStartupPacketTest, DatabaseDefaultsToUserWhenNotSupplied) {
    auto o = RunChildAndSend([](int fd) { WriteStartupPacket(fd, 0x00030000, {{"user", "bob"}}); });
    ASSERT_TRUE(o.exited);
    EXPECT_TRUE(o.result.valid);
    EXPECT_EQ(o.result.user, "bob");
    EXPECT_EQ(o.result.database, "bob");
}

TEST(ProcessStartupPacketTest, DatabaseUsedWhenSupplied) {
    auto o = RunChildAndSend([](int fd) {
        WriteStartupPacket(fd, 0x00030000, {{"user", "alice"}, {"database", "testdb"}});
    });
    ASSERT_TRUE(o.exited);
    EXPECT_TRUE(o.result.valid);
    EXPECT_EQ(o.result.user, "alice");
    EXPECT_EQ(o.result.database, "testdb");
}

TEST(ProcessStartupPacketTest, ParsesApplicationName) {
    auto o = RunChildAndSend([](int fd) {
        WriteStartupPacket(fd, 0x00030000, {{"user", "alice"}, {"application_name", "psql_test"}});
    });
    ASSERT_TRUE(o.exited);
    EXPECT_TRUE(o.result.valid);
    EXPECT_EQ(o.result.application_name, "psql_test");
}

// --- SSL / GSS preamble handling ---

TEST(ProcessStartupPacketTest, HandlesSslRequestPreamble) {
    auto o = RunChildAndSend([](int fd) {
        // First: SSLRequest — server should reply 'N' and continue.
        WriteSpecialPacket(fd, pgcpp::server::kNegotiateSslCode);
        // Read the 'N' reply.
        char reply = 0;
        ReadByte(fd, &reply);
        EXPECT_EQ(reply, 'N');
        // Then send the real startup packet.
        WriteStartupPacket(fd, 0x00030000, {{"user", "carol"}});
    });
    ASSERT_TRUE(o.exited);
    EXPECT_TRUE(o.result.valid);
    EXPECT_EQ(o.result.user, "carol");
}

TEST(ProcessStartupPacketTest, HandlesGssRequestPreamble) {
    auto o = RunChildAndSend([](int fd) {
        WriteSpecialPacket(fd, pgcpp::server::kNegotiateGssCode);
        char reply = 0;
        ReadByte(fd, &reply);
        EXPECT_EQ(reply, 'N');
        WriteStartupPacket(fd, 0x00030000, {{"user", "dave"}});
    });
    ASSERT_TRUE(o.exited);
    EXPECT_TRUE(o.result.valid);
    EXPECT_EQ(o.result.user, "dave");
}

TEST(ProcessStartupPacketTest, HandlesSslThenGssThenStartup) {
    auto o = RunChildAndSend([](int fd) {
        WriteSpecialPacket(fd, pgcpp::server::kNegotiateSslCode);
        char r1 = 0;
        ReadByte(fd, &r1);
        WriteSpecialPacket(fd, pgcpp::server::kNegotiateGssCode);
        char r2 = 0;
        ReadByte(fd, &r2);
        WriteStartupPacket(fd, 0x00030000, {{"user", "eve"}});
    });
    ASSERT_TRUE(o.exited);
    EXPECT_TRUE(o.result.valid);
    EXPECT_EQ(o.result.user, "eve");
}

// --- CancelRequest ---

TEST(ProcessStartupPacketTest, CancelRequestReturnsInvalid) {
    auto o = RunChildAndSend([](int fd) { WriteCancelPacket(fd, 12345, 67890); });
    ASSERT_TRUE(o.exited);
    EXPECT_FALSE(o.result.valid);
}

// --- EOF / error ---

TEST(ProcessStartupPacketTest, EofReturnsInvalid) {
    auto o = RunChildAndSend([](int fd) {
        // Close immediately without sending anything.
        (void)fd;
    });
    ASSERT_TRUE(o.exited);
    EXPECT_FALSE(o.result.valid);
}
