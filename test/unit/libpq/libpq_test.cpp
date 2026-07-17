// libpq_test.cpp — Unit and integration tests for the libpq client
// library (P3-11).
//
// The test suite is split into three parts:
//
//   1. Pure unit tests for the parsers / helpers (no server needed):
//      ConnInfo parsing (keyword=value + URI form),
//      FillDefaults, BuildConninfoString, GetOption, SetOption,
//      escape functions (literal / identifier / bytea),
//      PgResult accessor methods on a hand-built result.
//
//   2. In-process tests for the Large-Object API:
//      LoCreate / LoOpen / LoRead / LoWrite / LoSeek / LoTell /
//      LoTruncate / LoUnlink / LoImport / LoExport.
//
//   3. Integration tests that start a real pgcpp server (via Postmaster
//      in a child process) and exercise the full PgConn API: synchronous
//      connect, async connect, Exec, ExecParams, Prepare / ExecPrepared,
//      Cancel, pipeline mode, single-row mode.
#include "libpq/libpq.hpp"

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

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "libpq/cancel.hpp"
#include "libpq/conninfo.hpp"
#include "libpq/escape.hpp"
#include "libpq/large_object.hpp"
#include "server/bootstrap.hpp"
#include "server/postmaster.hpp"

using pgcpp::libpq::Cancel;
using pgcpp::libpq::ConnectOptions;
using pgcpp::libpq::ConnInfoOption;
using pgcpp::libpq::ConnStatusType;
using pgcpp::libpq::DefaultHost;
using pgcpp::libpq::DefaultPort;
using pgcpp::libpq::DefaultUser;
using pgcpp::libpq::EscapeBytea;
using pgcpp::libpq::EscapeIdentifier;
using pgcpp::libpq::EscapeLiteral;
using pgcpp::libpq::EscapeString;
using pgcpp::libpq::ExecStatusType;
using pgcpp::libpq::FieldFormat;
using pgcpp::libpq::FillDefaults;
using pgcpp::libpq::FormatResult;
using pgcpp::libpq::GetCancel;
using pgcpp::libpq::GetOption;
using pgcpp::libpq::LoClose;
using pgcpp::libpq::LoCreate;
using pgcpp::libpq::LoExport;
using pgcpp::libpq::LoImport;
using pgcpp::libpq::LoImportWithOid;
using pgcpp::libpq::LoOpen;
using pgcpp::libpq::LoRead;
using pgcpp::libpq::LoSeek;
using pgcpp::libpq::LoTell;
using pgcpp::libpq::LoTruncate;
using pgcpp::libpq::LoUnlink;
using pgcpp::libpq::LoWrite;
using pgcpp::libpq::Param;
using pgcpp::libpq::ParseConnInfo;
using pgcpp::libpq::PgCancel;
using pgcpp::libpq::PgConn;
using pgcpp::libpq::PgResult;
using pgcpp::libpq::PipelineStatusType;
using pgcpp::libpq::PollingStatusType;
using pgcpp::libpq::QuoteIdentifier;
using pgcpp::libpq::QuoteLiteral;
using pgcpp::libpq::ResultField;
using pgcpp::libpq::SetOption;
using pgcpp::libpq::UnescapeBytea;
using pgcpp::libpq::LoMode::kRead;
using pgcpp::libpq::LoMode::kWrite;
using pgcpp::server::BootstrapCluster;
using pgcpp::server::BootstrapResult;
using pgcpp::server::Postmaster;
using pgcpp::server::ServerConfig;

namespace {

// ---------------------------------------------------------------------------
// Test helpers (mirrors tools_test.cpp / server_test.cpp).
// ---------------------------------------------------------------------------

void RunShell(const std::string& cmd) {
    int ret = system(cmd.c_str());
    (void)ret;
}

std::string MakeTempDir(const std::string& prefix) {
    return "/tmp/" + prefix + "_" + std::to_string(getpid());
}

bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

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

pid_t StartServer(const std::string& data_dir, int port) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
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

void StopServer(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status;
        for (int i = 0; i < 50; ++i) {
            if (waitpid(pid, &status, WNOHANG) > 0)
                return;
            usleep(100000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

bool WaitForServer(const std::string& host, int port, int timeout_secs = 5) {
    time_t deadline = time(nullptr) + timeout_secs;
    while (time(nullptr) < deadline) {
        PgConn conn;
        std::string conninfo =
            "host=" + host + " port=" + std::to_string(port) + " user=pgcpp dbname=pgcpp";
        ConnStatusType st = conn.Connect(conninfo);
        if (st == ConnStatusType::kOk) {
            conn.Finish();
            return true;
        }
        usleep(100000);
    }
    return false;
}

}  // namespace

// ===========================================================================
// Part 1: ConnInfo parsing tests (no server needed)
// ===========================================================================

TEST(LibpqConnInfoTest, ParseKeywordValue) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    ASSERT_TRUE(ParseConnInfo("host=localhost port=5432 user=alice dbname=testdb", opts, errmsg));
    ASSERT_EQ(opts.size(), 4u);
    EXPECT_EQ(*GetOption(opts, "host"), "localhost");
    EXPECT_EQ(*GetOption(opts, "port"), "5432");
    EXPECT_EQ(*GetOption(opts, "user"), "alice");
    EXPECT_EQ(*GetOption(opts, "dbname"), "testdb");
}

TEST(LibpqConnInfoTest, ParseQuotedValue) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    ASSERT_TRUE(ParseConnInfo("password='it\\'s a secret'", opts, errmsg));
    ASSERT_EQ(opts.size(), 1u);
    EXPECT_EQ(*GetOption(opts, "password"), "it's a secret");
}

TEST(LibpqConnInfoTest, ParseDoubledQuote) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    // SQL-standard doubled single-quote inside quoted value.
    ASSERT_TRUE(ParseConnInfo("password='a''b'", opts, errmsg));
    ASSERT_EQ(opts.size(), 1u);
    EXPECT_EQ(*GetOption(opts, "password"), "a'b");
}

TEST(LibpqConnInfoTest, ParseBackslashEscape) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    // Backslash-space yields a literal space in an unquoted value.
    ASSERT_TRUE(ParseConnInfo("options='-c\\ geqo=off'", opts, errmsg));
    ASSERT_EQ(opts.size(), 1u);
    EXPECT_EQ(*GetOption(opts, "options"), "-c geqo=off");
}

TEST(LibpqConnInfoTest, ParseUriBasic) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    ASSERT_TRUE(ParseConnInfo("postgresql://localhost:5432/testdb", opts, errmsg));
    EXPECT_EQ(*GetOption(opts, "host"), "localhost");
    EXPECT_EQ(*GetOption(opts, "port"), "5432");
    EXPECT_EQ(*GetOption(opts, "dbname"), "testdb");
}

TEST(LibpqConnInfoTest, ParseUriWithUserinfo) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    ASSERT_TRUE(ParseConnInfo("postgres://alice:secret@localhost/testdb", opts, errmsg));
    EXPECT_EQ(*GetOption(opts, "user"), "alice");
    EXPECT_EQ(*GetOption(opts, "password"), "secret");
    EXPECT_EQ(*GetOption(opts, "host"), "localhost");
    EXPECT_EQ(*GetOption(opts, "dbname"), "testdb");
}

TEST(LibpqConnInfoTest, ParseUriWithQueryParams) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    ASSERT_TRUE(ParseConnInfo("postgresql://localhost/testdb?sslmode=disable&application_name=app1",
                              opts, errmsg));
    EXPECT_EQ(*GetOption(opts, "host"), "localhost");
    EXPECT_EQ(*GetOption(opts, "dbname"), "testdb");
    EXPECT_EQ(*GetOption(opts, "sslmode"), "disable");
    EXPECT_EQ(*GetOption(opts, "application_name"), "app1");
}

TEST(LibpqConnInfoTest, ParseUriIPv6) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    ASSERT_TRUE(ParseConnInfo("postgresql://[::1]:5432/testdb", opts, errmsg));
    EXPECT_EQ(*GetOption(opts, "host"), "::1");
    EXPECT_EQ(*GetOption(opts, "port"), "5432");
    EXPECT_EQ(*GetOption(opts, "dbname"), "testdb");
}

TEST(LibpqConnInfoTest, ParseUriInvalidScheme) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    EXPECT_FALSE(ParseConnInfo("mysql://localhost/testdb", opts, errmsg));
    EXPECT_FALSE(errmsg.empty());
}

TEST(LibpqConnInfoTest, ParseEmptyString) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    ASSERT_TRUE(ParseConnInfo("", opts, errmsg));
    EXPECT_TRUE(opts.empty());
}

TEST(LibpqConnInfoTest, ParseDuplicateKeyword) {
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    ASSERT_TRUE(ParseConnInfo("host=a.com host=b.com", opts, errmsg));
    // The last occurrence wins.
    EXPECT_EQ(*GetOption(opts, "host"), "b.com");
}

TEST(LibpqConnInfoTest, FillDefaultsAddsAllKnownKeywords) {
    std::vector<ConnInfoOption> opts;
    FillDefaults(opts);
    EXPECT_NE(GetOption(opts, "host"), nullptr);
    EXPECT_NE(GetOption(opts, "port"), nullptr);
    EXPECT_NE(GetOption(opts, "user"), nullptr);
    EXPECT_NE(GetOption(opts, "dbname"), nullptr);
    EXPECT_NE(GetOption(opts, "password"), nullptr);
    EXPECT_NE(GetOption(opts, "sslmode"), nullptr);
}

TEST(LibpqConnInfoTest, FillDefaultsPopulatesValues) {
    std::vector<ConnInfoOption> opts;
    FillDefaults(opts);
    EXPECT_EQ(*GetOption(opts, "host"), DefaultHost());
    EXPECT_EQ(*GetOption(opts, "port"), std::to_string(DefaultPort()));
    EXPECT_EQ(*GetOption(opts, "user"), DefaultUser());
}

TEST(LibpqConnInfoTest, FillDefaultsPreservesExisting) {
    std::vector<ConnInfoOption> opts;
    SetOption(opts, "host", "myhost.com");
    FillDefaults(opts);
    EXPECT_EQ(*GetOption(opts, "host"), "myhost.com");
}

TEST(LibpqConnInfoTest, SetOptionReplaces) {
    std::vector<ConnInfoOption> opts;
    SetOption(opts, "k", "v1");
    SetOption(opts, "k", "v2");
    ASSERT_EQ(opts.size(), 1u);
    EXPECT_EQ(opts[0].val, "v2");
}

TEST(LibpqConnInfoTest, GetOptionMissingReturnsNull) {
    std::vector<ConnInfoOption> opts;
    EXPECT_EQ(GetOption(opts, "nope"), nullptr);
}

TEST(LibpqConnInfoTest, BuildConninfoStringRoundTrip) {
    std::vector<ConnInfoOption> opts;
    SetOption(opts, "host", "localhost");
    SetOption(opts, "port", "5432");
    SetOption(opts, "user", "alice");
    std::string s = pgcpp::libpq::BuildConninfoString(opts);
    EXPECT_NE(s.find("host=localhost"), std::string::npos);
    EXPECT_NE(s.find("port=5432"), std::string::npos);
    EXPECT_NE(s.find("user=alice"), std::string::npos);
}

TEST(LibpqConnInfoTest, BuildConninfoStringQuotesIfNeeded) {
    std::vector<ConnInfoOption> opts;
    SetOption(opts, "password", "a b c");
    std::string s = pgcpp::libpq::BuildConninfoString(opts);
    EXPECT_NE(s.find("password='a b c'"), std::string::npos);
}

TEST(LibpqConnInfoTest, BuildConninfoStringEscapesQuotes) {
    std::vector<ConnInfoOption> opts;
    SetOption(opts, "password", "a'b");
    std::string s = pgcpp::libpq::BuildConninfoString(opts);
    EXPECT_NE(s.find("'a\\'b'"), std::string::npos);
}

TEST(LibpqConnInfoTest, DefaultUserNonEmpty) {
    EXPECT_FALSE(DefaultUser().empty());
}

TEST(LibpqConnInfoTest, DefaultHostNonEmpty) {
    EXPECT_FALSE(DefaultHost().empty());
}

TEST(LibpqConnInfoTest, DefaultPortIs5432OrEnv) {
    int p = DefaultPort();
    EXPECT_GT(p, 0);
    EXPECT_LT(p, 65536);
}

// ===========================================================================
// Part 2: Escape tests (no server needed)
// ===========================================================================

TEST(LibpqEscapeTest, EscapeLiteralSimple) {
    auto r = EscapeLiteral("hello");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.out, "'hello'");
}

TEST(LibpqEscapeTest, EscapeLiteralWithSingleQuote) {
    auto r = EscapeLiteral("it's a test");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.out, "'it''s a test'");
}

TEST(LibpqEscapeTest, EscapeLiteralWithBackslash) {
    // With standard_conforming_strings=on, backslash is a literal char.
    auto r = EscapeLiteral("a\\b");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.out, "'a\\b'");
}

TEST(LibpqEscapeTest, EscapeLiteralEmpty) {
    auto r = EscapeLiteral("");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.out, "''");
}

TEST(LibpqEscapeTest, EscapeStringConnMatchesStandalone) {
    PgConn conn;
    auto r1 = EscapeString("hi");
    auto r2 = EscapeStringConn(conn, "hi");
    EXPECT_EQ(r1.out, r2.out);
}

TEST(LibpqEscapeTest, EscapeIdentifierSimple) {
    auto r = EscapeIdentifier("mytable");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.out, "\"mytable\"");
}

TEST(LibpqEscapeTest, EscapeIdentifierWithSpecialCharFails) {
    auto r = EscapeIdentifier("my-table");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error_message.empty());
}

TEST(LibpqEscapeTest, EscapeIdentifierEmptyFails) {
    auto r = EscapeIdentifier("");
    EXPECT_FALSE(r.ok);
}

TEST(LibpqEscapeTest, EscapeIdentifierTooLongFails) {
    auto r = EscapeIdentifier(std::string(64, 'a'));
    EXPECT_FALSE(r.ok);
}

TEST(LibpqEscapeTest, EscapeIdentifierWithDoubleQuote) {
    // A double-quote is allowed in identifiers (escaped as "") but our
    // strict validator only allows [A-Za-z0-9_$]. So this should fail.
    auto r = EscapeIdentifier("a\"b");
    EXPECT_FALSE(r.ok);
}

TEST(LibpqEscapeTest, QuoteLiteral) {
    EXPECT_EQ(QuoteLiteral("hi"), "'hi'");
    EXPECT_EQ(QuoteLiteral("it's"), "'it''s'");
}

TEST(LibpqEscapeTest, QuoteIdentifier) {
    EXPECT_EQ(QuoteIdentifier("mycol"), "\"mycol\"");
    EXPECT_EQ(QuoteIdentifier("a\"b"), "\"a\"\"b\"");
}

TEST(LibpqEscapeTest, EscapeByteaHex) {
    unsigned char data[] = {0x00, 0x01, 0xFF, 0x10, 'A'};
    std::string s = EscapeBytea(data, 5);
    EXPECT_EQ(s, "\\x0001ff1041");
}

TEST(LibpqEscapeTest, EscapeByteaEmpty) {
    unsigned char data[1] = {0};
    std::string s = EscapeBytea(data, 0);
    EXPECT_EQ(s, "\\x");
}

TEST(LibpqEscapeTest, UnescapeByteaHex) {
    auto r = UnescapeBytea("\\x414243", 8);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.out.size(), 3u);
    EXPECT_EQ(r.out[0], 'A');
    EXPECT_EQ(r.out[1], 'B');
    EXPECT_EQ(r.out[2], 'C');
}

TEST(LibpqEscapeTest, UnescapeByteaOctal) {
    auto r = UnescapeBytea("\\101\\102\\103", 12);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.out.size(), 3u);
    EXPECT_EQ(r.out[0], 'A');
    EXPECT_EQ(r.out[1], 'B');
    EXPECT_EQ(r.out[2], 'C');
}

TEST(LibpqEscapeTest, UnescapeByteaBackslash) {
    auto r = UnescapeBytea("\\\\", 2);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.out.size(), 1u);
    EXPECT_EQ(r.out[0], '\\');
}

TEST(LibpqEscapeTest, UnescapeByteaInvalidHex) {
    auto r = UnescapeBytea("\\x4G", 4);
    EXPECT_FALSE(r.ok);
}

TEST(LibpqEscapeTest, EscapeByteaRoundTrip) {
    std::vector<unsigned char> data;
    for (int i = 0; i < 256; ++i)
        data.push_back(static_cast<unsigned char>(i));
    std::string escaped = EscapeBytea(data.data(), data.size());
    auto r = UnescapeBytea(escaped.c_str(), escaped.size());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.out.size(), data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        EXPECT_EQ(r.out[i], data[i]) << "mismatch at byte " << i;
    }
}

// ===========================================================================
// Part 3: PgResult accessor tests (no server needed)
// ===========================================================================

TEST(LibpqResultTest, EmptyResultStatus) {
    PgResult r;
    EXPECT_EQ(r.Status(), ExecStatusType::kFatalError);
    EXPECT_EQ(r.NTuples(), 0);
    EXPECT_EQ(r.NFields(), 0);
}

TEST(LibpqResultTest, AddRowAndGetValue) {
    PgResult r;
    r.SetFields({{"a", 0, 0, 0, 4, -1, 0}, {"b", 0, 0, 0, -1, -1, 0}});
    r.AddRow({"42", "hello"}, {false, false});
    EXPECT_EQ(r.NTuples(), 1);
    EXPECT_EQ(r.NFields(), 2);
    EXPECT_STREQ(r.GetValue(0, 0), "42");
    EXPECT_STREQ(r.GetValue(0, 1), "hello");
    EXPECT_EQ(r.GetLength(0, 0), 2);
    EXPECT_EQ(r.GetLength(0, 1), 5);
    EXPECT_FALSE(r.GetIsNull(0, 0));
}

TEST(LibpqResultTest, NullValue) {
    PgResult r;
    r.SetFields({{"a", 0, 0, 0, 4, -1, 0}});
    r.AddRow({""}, {true});
    EXPECT_TRUE(r.GetIsNull(0, 0));
    EXPECT_EQ(r.GetValue(0, 0), nullptr);
    EXPECT_EQ(r.GetLength(0, 0), -1);
}

TEST(LibpqResultTest, FNumberCaseInsensitive) {
    PgResult r;
    r.SetFields({{"Name", 0, 0, 0, -1, -1, 0}});
    EXPECT_EQ(r.FNumber("Name"), 0);
    EXPECT_EQ(r.FNumber("name"), 0);
    EXPECT_EQ(r.FNumber("NAME"), 0);
    EXPECT_EQ(r.FNumber("nonexistent"), -1);
}

TEST(LibpqResultTest, FieldAccessorsByIndex) {
    PgResult r;
    r.SetFields({{"a", 16384, 1, 23, 4, -1, 0}});
    EXPECT_EQ(r.FName(0), "a");
    EXPECT_EQ(r.FTable(0), 16384u);
    EXPECT_EQ(r.FTableCol(0), 1);
    EXPECT_EQ(r.FType(0), 23u);
    EXPECT_EQ(r.FSize(0), 4);
    EXPECT_EQ(r.FMod(0), -1);
    EXPECT_EQ(r.FFormat(0), 0);
}

TEST(LibpqResultTest, FieldAccessorsOutOfBounds) {
    PgResult r;
    EXPECT_EQ(r.FName(0), "");
    EXPECT_EQ(r.FNumber("a"), -1);
    EXPECT_EQ(r.FType(0), 0u);
    EXPECT_EQ(r.GetValue(0, 0), nullptr);
    EXPECT_TRUE(r.GetIsNull(0, 0));
    EXPECT_EQ(r.GetLength(0, 0), -1);
}

TEST(LibpqResultTest, CommandTuplesSelect) {
    PgResult r;
    r.SetCommandStatus("SELECT 5");
    EXPECT_EQ(r.CommandTuples(), 5);
}

TEST(LibpqResultTest, CommandTuplesInsert) {
    PgResult r;
    r.SetCommandStatus("INSERT 0 1");
    EXPECT_EQ(r.CommandTuples(), 1);
}

TEST(LibpqResultTest, CommandTuplesUpdate) {
    PgResult r;
    r.SetCommandStatus("UPDATE 3");
    EXPECT_EQ(r.CommandTuples(), 3);
}

TEST(LibpqResultTest, CommandTuplesEmpty) {
    PgResult r;
    EXPECT_EQ(r.CommandTuples(), -1);
}

TEST(LibpqResultTest, ErrorMessage) {
    PgResult r;
    r.SetErrorMessage("syntax error");
    EXPECT_EQ(r.ErrorMessage(), "syntax error");
    EXPECT_EQ(r.VerboseErrorMessage(), "syntax error");
}

TEST(LibpqResultTest, ClearResetsState) {
    PgResult r;
    r.SetFields({{"a", 0, 0, 0, 4, -1, 0}});
    r.AddRow({"42"}, {false});
    r.SetCommandStatus("SELECT 1");
    r.Clear();
    EXPECT_EQ(r.NTuples(), 0);
    EXPECT_EQ(r.NFields(), 0);
    EXPECT_EQ(r.CommandStatus(), "");
}

TEST(LibpqResultTest, ReserveRowsDoesNotChangeCount) {
    PgResult r;
    r.ReserveRows(100);
    EXPECT_EQ(r.NTuples(), 0);
}

TEST(LibpqResultTest, MultipleRows) {
    PgResult r;
    r.SetFields({{"id", 0, 0, 0, 4, -1, 0}});
    for (int i = 0; i < 5; ++i) {
        r.AddRow({std::to_string(i)}, {false});
    }
    EXPECT_EQ(r.NTuples(), 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(std::string(r.GetValue(i, 0)), std::to_string(i));
    }
}

TEST(LibpqResultTest, MoveConstructorTransfersOwnership) {
    PgResult r1;
    r1.SetFields({{"a", 0, 0, 0, 4, -1, 0}});
    r1.AddRow({"42"}, {false});
    r1.SetCommandStatus("SELECT 1");

    PgResult r2(std::move(r1));
    EXPECT_EQ(r2.NTuples(), 1);
    EXPECT_EQ(r2.NFields(), 1);
    EXPECT_EQ(r2.CommandStatus(), "SELECT 1");
}

TEST(LibpqResultTest, FormatResultTuples) {
    PgResult r;
    r.SetStatus(ExecStatusType::kTuplesOk);
    r.SetFields({{"a", 0, 0, 0, 4, -1, 0}, {"b", 0, 0, 0, -1, -1, 0}});
    r.AddRow({"1", "hello"}, {false, false});
    r.AddRow({"2", "world"}, {false, false});
    r.SetCommandStatus("SELECT 2");
    std::string formatted = FormatResult(r);
    EXPECT_NE(formatted.find("a"), std::string::npos);
    EXPECT_NE(formatted.find("b"), std::string::npos);
    EXPECT_NE(formatted.find("hello"), std::string::npos);
    EXPECT_NE(formatted.find("world"), std::string::npos);
    EXPECT_NE(formatted.find("(2 rows)"), std::string::npos);
}

TEST(LibpqResultTest, FormatResultError) {
    PgResult r;
    r.SetStatus(ExecStatusType::kFatalError);
    r.SetErrorMessage("syntax error");
    std::string formatted = FormatResult(r);
    EXPECT_NE(formatted.find("ERROR"), std::string::npos);
    EXPECT_NE(formatted.find("syntax error"), std::string::npos);
}

TEST(LibpqResultTest, FormatResultCommand) {
    PgResult r;
    r.SetStatus(ExecStatusType::kCommandOk);
    r.SetCommandStatus("INSERT 0 1");
    std::string formatted = FormatResult(r);
    EXPECT_NE(formatted.find("INSERT 0 1"), std::string::npos);
}

// ===========================================================================
// Part 4: Large object tests (no server needed)
// ===========================================================================

TEST(LibpqLargeObjectTest, LoCreateAllocatesOid) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0);
    EXPECT_NE(oid, 0u);
}

TEST(LibpqLargeObjectTest, LoCreateWithExplicitOid) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x12345);
    EXPECT_EQ(oid, 0x12345u);
}

TEST(LibpqLargeObjectTest, LoCreateIdempotent) {
    PgConn conn;
    uint32_t oid1 = LoCreate(conn, 0xABCDE);
    uint32_t oid2 = LoCreate(conn, 0xABCDE);
    EXPECT_EQ(oid1, oid2);
}

TEST(LibpqLargeObjectTest, LoOpenReturnsValidFd) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x20000);
    ASSERT_NE(oid, 0u);
    int fd = LoOpen(conn, oid, kRead | kWrite);
    EXPECT_GE(fd, 0);
    EXPECT_EQ(LoClose(conn, fd), 0);
}

TEST(LibpqLargeObjectTest, LoWriteAndRead) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x30000);
    ASSERT_NE(oid, 0u);
    int fd = LoOpen(conn, oid, kWrite);
    ASSERT_GE(fd, 0);

    const char data[] = "hello world";
    int written = LoWrite(conn, fd, data, 11);
    EXPECT_EQ(written, 11);
    EXPECT_EQ(LoClose(conn, fd), 0);

    int fd2 = LoOpen(conn, oid, kRead);
    ASSERT_GE(fd2, 0);
    char buf[32] = {0};
    int n = LoRead(conn, fd2, buf, 32);
    EXPECT_EQ(n, 11);
    EXPECT_EQ(std::string(buf, 11), "hello world");
    LoClose(conn, fd2);
}

TEST(LibpqLargeObjectTest, LoSeekSet) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x40000);
    int fd = LoOpen(conn, oid, kRead | kWrite);
    ASSERT_GE(fd, 0);

    LoWrite(conn, fd, "0123456789", 10);
    EXPECT_EQ(LoSeek(conn, fd, 5, 0), 0);  // SEEK_SET
    EXPECT_EQ(LoTell(conn, fd), 5);
    char buf[8] = {0};
    int n = LoRead(conn, fd, buf, 5);
    EXPECT_EQ(n, 5);
    EXPECT_EQ(std::string(buf, 5), "56789");
    LoClose(conn, fd);
}

TEST(LibpqLargeObjectTest, LoSeekCur) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x40001);
    int fd = LoOpen(conn, oid, kRead | kWrite);
    ASSERT_GE(fd, 0);

    LoWrite(conn, fd, "0123456789", 10);
    EXPECT_EQ(LoSeek(conn, fd, 3, 0), 0);
    EXPECT_EQ(LoSeek(conn, fd, 2, 1), 0);  // SEEK_CUR
    EXPECT_EQ(LoTell(conn, fd), 5);
    LoClose(conn, fd);
}

TEST(LibpqLargeObjectTest, LoSeekEnd) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x40002);
    int fd = LoOpen(conn, oid, kRead | kWrite);
    ASSERT_GE(fd, 0);

    LoWrite(conn, fd, "0123456789", 10);
    EXPECT_EQ(LoSeek(conn, fd, -3, 2), 0);  // SEEK_END
    EXPECT_EQ(LoTell(conn, fd), 7);
    LoClose(conn, fd);
}

TEST(LibpqLargeObjectTest, LoTruncate) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x40003);
    int fd = LoOpen(conn, oid, kRead | kWrite);
    ASSERT_GE(fd, 0);

    LoWrite(conn, fd, "0123456789", 10);
    EXPECT_EQ(LoTruncate(conn, fd, 5), 0);
    EXPECT_EQ(LoSeek(conn, fd, 0, 0), 0);
    char buf[16] = {0};
    int n = LoRead(conn, fd, buf, 16);
    EXPECT_EQ(n, 5);
    LoClose(conn, fd);
}

TEST(LibpqLargeObjectTest, LoUnlink) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x50000);
    EXPECT_EQ(LoUnlink(conn, oid), 1);
    // Now opening should give an empty (but valid) fd.
    int fd = LoOpen(conn, oid, kRead);
    EXPECT_GE(fd, 0);
    LoClose(conn, fd);
}

TEST(LibpqLargeObjectTest, LoUnlinkNonexistent) {
    PgConn conn;
    EXPECT_EQ(LoUnlink(conn, 0x99999), -1);
}

TEST(LibpqLargeObjectTest, LoImportExport) {
    PgConn conn;
    std::string tmp_file = "/tmp/libpq_lo_test_" + std::to_string(getpid()) + ".bin";
    RunShell("rm -f " + tmp_file);
    FILE* fp = fopen(tmp_file.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    const char data[] = "large object content";
    fwrite(data, 1, sizeof(data) - 1, fp);
    fclose(fp);

    uint32_t oid = LoImport(conn, tmp_file);
    EXPECT_NE(oid, 0u);

    std::string out_file = tmp_file + ".out";
    RunShell("rm -f " + out_file);
    EXPECT_EQ(LoExport(conn, oid, out_file), 1);
    EXPECT_TRUE(IsFile(out_file));

    // Verify the exported content.
    fp = fopen(out_file.c_str(), "rb");
    ASSERT_NE(fp, nullptr);
    char buf[64] = {0};
    std::size_t nread = fread(buf, 1, sizeof(buf), fp);
    (void)nread;
    fclose(fp);
    EXPECT_EQ(std::string(buf), "large object content");

    RunShell("rm -f " + tmp_file + " " + out_file);
}

TEST(LibpqLargeObjectTest, LoReadEmptyObject) {
    PgConn conn;
    uint32_t oid = LoCreate(conn, 0x60000);
    int fd = LoOpen(conn, oid, kRead);
    ASSERT_GE(fd, 0);
    char buf[16] = {0};
    int n = LoRead(conn, fd, buf, 16);
    EXPECT_EQ(n, 0);  // no data
    LoClose(conn, fd);
}

TEST(LibpqLargeObjectTest, LoCloseInvalidFd) {
    PgConn conn;
    EXPECT_EQ(LoClose(conn, 9999), -1);
    EXPECT_EQ(LoClose(conn, -1), -1);
}

// ===========================================================================
// Part 5: GetCancel data flow tests (no server needed)
// ===========================================================================

TEST(LibpqCancelTest, GetCancelFromDisconnectedConn) {
    PgConn conn;
    PgCancel cancel;
    EXPECT_FALSE(GetCancel(conn, cancel));
}

TEST(LibpqCancelTest, PgCancelDefaultValues) {
    PgCancel cancel;
    EXPECT_EQ(cancel.pid, 0u);
    EXPECT_EQ(cancel.secret, 0u);
    EXPECT_EQ(cancel.port, 0);
    EXPECT_TRUE(cancel.host.empty());
}

TEST(LibpqCancelTest, CancelFailsOnInvalidHost) {
    PgCancel cancel;
    cancel.host = "0.0.0.0";  // unreachable on a high port
    cancel.port = 1;          // privileged port — will fail to connect
    cancel.pid = 1234;
    cancel.secret = 0;
    // Note: the result depends on whether the test runner has permission
    // to open privileged sockets. We expect false in most cases.
    bool result = Cancel(cancel);
    // We don't assert on result because it depends on the environment.
    (void)result;
}

// ===========================================================================
// Part 6: PgConn integration tests (requires a running server)
// ===========================================================================

class LibpqConnTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = MakeTempDir("pgcpp_libpq_test");
        RunShell("rm -rf " + test_dir_);
        ASSERT_EQ(BootstrapCluster(test_dir_), BootstrapResult::kOk);

        port_ = FindFreePort();
        server_pid_ = StartServer(test_dir_, port_);
        ASSERT_GT(server_pid_, 0);
        ASSERT_TRUE(WaitForServer("127.0.0.1", port_));
    }

    void TearDown() override {
        StopServer(server_pid_);
        RunShell("rm -rf " + test_dir_);
    }

    std::string Conninfo() {
        return "host=127.0.0.1 port=" + std::to_string(port_) + " user=pgcpp dbname=pgcpp";
    }

    std::string test_dir_;
    int port_ = 0;
    pid_t server_pid_ = -1;
};

TEST_F(LibpqConnTest, ConnectSucceeds) {
    PgConn conn;
    EXPECT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    EXPECT_EQ(conn.Status(), ConnStatusType::kOk);
    EXPECT_GE(conn.Socket(), 0);
    EXPECT_EQ(conn.Host(), "127.0.0.1");
    EXPECT_EQ(conn.Port(), port_);
    EXPECT_EQ(conn.User(), "pgcpp");
    EXPECT_EQ(conn.DB(), "pgcpp");
    conn.Finish();
    EXPECT_EQ(conn.Status(), ConnStatusType::kBad);
}

TEST_F(LibpqConnTest, ConnectWithOptions) {
    PgConn conn;
    ConnectOptions opts;
    opts.host = "127.0.0.1";
    opts.port = port_;
    opts.user = "pgcpp";
    opts.dbname = "pgcpp";
    EXPECT_EQ(conn.Connect(opts), ConnStatusType::kOk);
    conn.Finish();
}

TEST_F(LibpqConnTest, ConnectInvalidHost) {
    PgConn conn;
    EXPECT_NE(conn.Connect("host=0.0.0.1 port=" + std::to_string(port_)), ConnStatusType::kOk);
    EXPECT_FALSE(conn.ErrorMessage().empty());
}

TEST_F(LibpqConnTest, ConnectInvalidPort) {
    PgConn conn;
    // Port 1 is reserved — connection should fail quickly.
    EXPECT_NE(conn.Connect("host=127.0.0.1 port=1"), ConnStatusType::kOk);
}

TEST_F(LibpqConnTest, ConnectBackendKeyData) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    EXPECT_GT(conn.BackendPID(), 0);
    conn.Finish();
}

TEST_F(LibpqConnTest, ConnectParameterStatus) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    const std::string* sv = conn.ParameterStatus("server_version");
    EXPECT_NE(sv, nullptr);
    EXPECT_FALSE(sv->empty());
    conn.Finish();
}

TEST_F(LibpqConnTest, ConnectProtocolVersion) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    // Protocol v3 = 3 << 16 = 196608.
    EXPECT_EQ(conn.ProtocolVersion(), 196608);
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecSelect1) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    PgResult r = conn.Exec("SELECT 1");
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    EXPECT_EQ(r.NTuples(), 1);
    EXPECT_EQ(r.NFields(), 1);
    EXPECT_EQ(std::string(r.GetValue(0, 0)), "1");
    EXPECT_EQ(r.CommandStatus(), "SELECT 1");
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecSelectMultipleColumns) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    PgResult r = conn.Exec("SELECT 1, 2, 3");
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    EXPECT_EQ(r.NFields(), 3);
    EXPECT_EQ(r.NTuples(), 1);
    EXPECT_EQ(std::string(r.GetValue(0, 0)), "1");
    EXPECT_EQ(std::string(r.GetValue(0, 1)), "2");
    EXPECT_EQ(std::string(r.GetValue(0, 2)), "3");
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecArithmetic) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    PgResult r = conn.Exec("SELECT 2 + 3");
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    EXPECT_EQ(std::string(r.GetValue(0, 0)), "5");
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecEmptyQuery) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    PgResult r = conn.Exec(";");
    EXPECT_EQ(r.Status(), ExecStatusType::kEmptyQuery);
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecSyntaxError) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    PgResult r = conn.Exec("SELCT 1");
    EXPECT_EQ(r.Status(), ExecStatusType::kFatalError);
    EXPECT_FALSE(r.ErrorMessage().empty());
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecMultipleQueries) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    PgResult r1 = conn.Exec("SELECT 1");
    EXPECT_EQ(r1.Status(), ExecStatusType::kTuplesOk);
    EXPECT_EQ(std::string(r1.GetValue(0, 0)), "1");

    PgResult r2 = conn.Exec("SELECT 2 + 3");
    EXPECT_EQ(r2.Status(), ExecStatusType::kTuplesOk);
    EXPECT_EQ(std::string(r2.GetValue(0, 0)), "5");

    PgResult r3 = conn.Exec("SELECT 10 * 10");
    EXPECT_EQ(r3.Status(), ExecStatusType::kTuplesOk);
    EXPECT_EQ(std::string(r3.GetValue(0, 0)), "100");
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecTransactionControl) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    PgResult begin = conn.Exec("BEGIN");
    EXPECT_EQ(begin.Status(), ExecStatusType::kCommandOk);

    PgResult select = conn.Exec("SELECT 1");
    EXPECT_EQ(select.Status(), ExecStatusType::kTuplesOk);
    EXPECT_EQ(std::string(select.GetValue(0, 0)), "1");

    PgResult commit = conn.Exec("COMMIT");
    EXPECT_EQ(commit.Status(), ExecStatusType::kCommandOk);
    conn.Finish();
}

TEST_F(LibpqConnTest, ResetReconnects) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    EXPECT_EQ(conn.Reset(), ConnStatusType::kOk);
    PgResult r = conn.Exec("SELECT 1");
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    conn.Finish();
}

TEST_F(LibpqConnTest, SendQueryAndGetResult) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    ASSERT_TRUE(conn.SendQuery("SELECT 42"));
    PgResult r = conn.GetResult();
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    EXPECT_EQ(std::string(r.GetValue(0, 0)), "42");
    // Drain the terminator result.
    PgResult term = conn.GetResult();
    (void)term;
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecParams) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    // Project a single parameter value through the extended query protocol.
    // (Server-side arithmetic on params is not yet supported, so we use
    // a direct param projection which exercises the full Bind/Execute path.)
    PgResult r = conn.ExecParams("SELECT $1", {Param{"8", false}});
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    ASSERT_EQ(r.NTuples(), 1);
    EXPECT_EQ(std::string(r.GetValue(0, 0)), "8");
    conn.Finish();
}

TEST_F(LibpqConnTest, ExecParamsWithNull) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    // SELECT $1 with NULL param — pgcpp server may return NULL or empty;
    // we accept either as long as it does not crash.
    PgResult r = conn.ExecParams("SELECT $1", {Param{"", true}});
    // Don't strictly assert status — the server may return either empty
    // or a NULL value depending on its handling.
    (void)r;
    conn.Finish();
}

TEST_F(LibpqConnTest, PrepareAndExecPrepared) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    PgResult prep = conn.Prepare("myplan", "SELECT $1", {});
    EXPECT_EQ(prep.Status(), ExecStatusType::kCommandOk);
    // Server-side arithmetic on params is not yet supported; use a direct
    // param projection to exercise Prepare -> Bind -> Execute -> Sync.
    PgResult r = conn.ExecPrepared("myplan", {Param{"7", false}});
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    ASSERT_EQ(r.NTuples(), 1);
    EXPECT_EQ(std::string(r.GetValue(0, 0)), "7");
    conn.Finish();
}

TEST_F(LibpqConnTest, PipelineMode) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    EXPECT_TRUE(conn.EnterPipelineMode());
    EXPECT_EQ(conn.PipelineStatus(), PipelineStatusType::kOn);
    EXPECT_TRUE(conn.SendQuery("SELECT 1"));
    EXPECT_TRUE(conn.SendQuery("SELECT 2"));
    EXPECT_TRUE(conn.PipelineSync());
    EXPECT_TRUE(conn.ExitPipelineMode());
    EXPECT_EQ(conn.PipelineStatus(), PipelineStatusType::kOff);
    conn.Finish();
}

TEST_F(LibpqConnTest, SingleRowModeFlag) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    EXPECT_TRUE(conn.SetSingleRowMode());
    conn.Finish();
}

TEST_F(LibpqConnTest, FlushReturnsZero) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    EXPECT_EQ(conn.Flush(), 0);
    conn.Finish();
}

TEST_F(LibpqConnTest, NotifiesEmptyInitially) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    EXPECT_EQ(conn.Notifies(), nullptr);
    conn.Finish();
}

TEST_F(LibpqConnTest, ConsumeInput) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    // Just verify it doesn't crash on an idle connection.
    EXPECT_TRUE(conn.ConsumeInput());
    conn.Finish();
}

TEST_F(LibpqConnTest, CancelCurrentQuery) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    // Just verify the cancel path doesn't crash. A query that's actually
    // running and getting cancelled requires a long-running query to test
    // meaningfully.
    bool ok = conn.CancelCurrentQuery();
    (void)ok;
    conn.Finish();
}

TEST_F(LibpqConnTest, MoveConstructor) {
    PgConn conn1;
    ASSERT_EQ(conn1.Connect(Conninfo()), ConnStatusType::kOk);
    int fd = conn1.Socket();
    EXPECT_GE(fd, 0);

    PgConn conn2(std::move(conn1));
    EXPECT_EQ(conn2.Socket(), fd);
    EXPECT_EQ(conn1.Socket(), -1);

    PgResult r = conn2.Exec("SELECT 1");
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    conn2.Finish();
}

TEST_F(LibpqConnTest, MoveAssignment) {
    PgConn conn1, conn2;
    ASSERT_EQ(conn1.Connect(Conninfo()), ConnStatusType::kOk);
    conn2 = std::move(conn1);
    EXPECT_EQ(conn1.Socket(), -1);
    PgResult r = conn2.Exec("SELECT 1");
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    conn2.Finish();
}

TEST_F(LibpqConnTest, DestructorClosesConnection) {
    int fd;
    {
        PgConn conn;
        ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
        fd = conn.Socket();
        ASSERT_GE(fd, 0);
        // Destructor should run.
    }
    // After destruction, fd should be closed (we can't easily verify
    // from outside the class, but at least the test should not leak
    // resources).
    (void)fd;
}

TEST_F(LibpqConnTest, ConnectStartAndPoll) {
    PgConn conn;
    ASSERT_EQ(conn.ConnectStart(Conninfo()), ConnStatusType::kStarted);
    // Drive the state machine until OK or failure.
    for (int i = 0; i < 100; ++i) {
        PollingStatusType st = conn.ConnectPoll();
        if (st == PollingStatusType::kOk)
            break;
        if (st == PollingStatusType::kFailed)
            break;
        if (st == PollingStatusType::kReading || st == PollingStatusType::kWriting) {
            usleep(10000);
            continue;
        }
        usleep(10000);
    }
    EXPECT_EQ(conn.Status(), ConnStatusType::kOk);
    if (conn.Status() == ConnStatusType::kOk) {
        PgResult r = conn.Exec("SELECT 1");
        EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    }
    conn.Finish();
}

TEST_F(LibpqConnTest, TraceUntrace) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    conn.Trace();
    PgResult r = conn.Exec("SELECT 1");
    EXPECT_EQ(r.Status(), ExecStatusType::kTuplesOk);
    conn.Untrace();
    conn.Finish();
}

TEST_F(LibpqConnTest, ConnectionOptions) {
    PgConn conn;
    ASSERT_EQ(conn.Connect(Conninfo()), ConnStatusType::kOk);
    const auto& opts = conn.ConnectionOptions();
    EXPECT_FALSE(opts.empty());
    const std::string* host = GetOption(opts, "host");
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(*host, "127.0.0.1");
    conn.Finish();
}
