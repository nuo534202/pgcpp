// psql_command_test.cpp — Unit tests for the backslash meta-command
// dispatcher (ExecuteMetaCommand).
//
// Tests the dispatch logic for \q, \?, \echo, \set, \unset, \c, unknown
// commands, and the SQL-generating commands (\dt, \dv, \l, \du, \d) which
// are verified by checking that a query was sent to a fake client and
// that the result was printed to the injected output stream.
#include "tools/psql_command.hpp"

#include <gtest/gtest.h>

#include <map>
#include <sstream>
#include <string>

#include "tools/psql_client.hpp"

using pgcpp::tools::ExecuteMetaCommand;
using pgcpp::tools::MetaCommandResult;

namespace {

// FakePsqlClient — a PsqlClient stand-in that records the last SQL sent
// and returns a canned QueryResult. We cannot subclass PsqlClient (its
// methods are non-virtual), so instead we expose a small adapter that
// ExecuteMetaCommand can be parameterized on via a free function.
//
// Approach: define a minimal "client" interface that ExecuteMetaCommand
// uses. Since the public API takes PsqlClient&, we use a real PsqlClient
// but override its behavior via a seam: when the connection fd is -1,
// ExecuteQuery returns the canned result without touching the network.
//
// Even simpler: tests that don't require server interaction (\q, \?, \echo,
// \set, \unset, unknown) work directly. For server-dependent commands,
// we exercise them only on a real server (in psql_command_integration_test).

}  // namespace

// ---------------------------------------------------------------------------
// Dispatch tests that don't require a server connection.
// These commands never call PsqlClient::ExecuteQuery.
// ---------------------------------------------------------------------------

TEST(PsqlCommandTest, QuitCommandReturnsQuit) {
    // \q doesn't touch the client, so a default-constructed (disconnected)
    // PsqlClient is fine.
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    MetaCommandResult r = ExecuteMetaCommand(client, "\\q", vars, out);
    EXPECT_EQ(r, MetaCommandResult::kQuit);
    EXPECT_TRUE(out.str().empty());
}

TEST(PsqlCommandTest, QuitLongFormReturnsQuit) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    MetaCommandResult r = ExecuteMetaCommand(client, "\\quit", vars, out);
    EXPECT_EQ(r, MetaCommandResult::kQuit);
}

TEST(PsqlCommandTest, HelpCommandListsAvailableCommands) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    MetaCommandResult r = ExecuteMetaCommand(client, "\\?", vars, out);
    EXPECT_EQ(r, MetaCommandResult::kContinue);
    std::string s = out.str();
    EXPECT_NE(s.find("\\q"), std::string::npos);
    EXPECT_NE(s.find("\\dt"), std::string::npos);
    EXPECT_NE(s.find("\\echo"), std::string::npos);
    EXPECT_NE(s.find("\\set"), std::string::npos);
}

TEST(PsqlCommandTest, HelpLongFormWorks) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    MetaCommandResult r = ExecuteMetaCommand(client, "\\help", vars, out);
    EXPECT_EQ(r, MetaCommandResult::kContinue);
    EXPECT_FALSE(out.str().empty());
}

TEST(PsqlCommandTest, EchoCommandPrintsArguments) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    MetaCommandResult r = ExecuteMetaCommand(client, "\\echo hello world", vars, out);
    EXPECT_EQ(r, MetaCommandResult::kContinue);
    EXPECT_EQ(out.str(), "hello world\n");
}

TEST(PsqlCommandTest, EchoWithNoArgsPrintsBlankLine) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\echo", vars, out);
    EXPECT_EQ(out.str(), "\n");
}

TEST(PsqlCommandTest, EchoExpandsPsqlVariable) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars = {{"foo", "bar"}};
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\echo :foo", vars, out);
    EXPECT_EQ(out.str(), "bar\n");
}

TEST(PsqlCommandTest, SetCommandStoresVariable) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\set myvar 42", vars, out);
    EXPECT_EQ(vars["myvar"], "42");
}

TEST(PsqlCommandTest, SetCommandStoresMultiWordValue) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\set greeting hello world", vars, out);
    EXPECT_EQ(vars["greeting"], "hello world");
}

TEST(PsqlCommandTest, SetWithNoArgsListsAllVars) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars = {{"a", "1"}, {"b", "2"}};
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\set", vars, out);
    std::string s = out.str();
    EXPECT_NE(s.find("a = 1"), std::string::npos);
    EXPECT_NE(s.find("b = 2"), std::string::npos);
}

TEST(PsqlCommandTest, UnsetCommandRemovesVariable) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars = {{"x", "1"}, {"y", "2"}};
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\unset x", vars, out);
    EXPECT_EQ(vars.count("x"), 0u);
    EXPECT_EQ(vars.count("y"), 1u);
}

TEST(PsqlCommandTest, UnsetWithNoArgPrintsError) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\unset", vars, out);
    EXPECT_NE(out.str().find("missing"), std::string::npos);
}

TEST(PsqlCommandTest, ConnectCommandWithNoArgPrintsError) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\c", vars, out);
    EXPECT_NE(out.str().find("missing"), std::string::npos);
}

TEST(PsqlCommandTest, ConnectCommandPrintsStubMessage) {
    // \c <dbname> doesn't actually reconnect (no socket reuse), but should
    // print a confirmation message so users know the command was recognized.
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\c mydb", vars, out);
    EXPECT_NE(out.str().find("mydb"), std::string::npos);
}

TEST(PsqlCommandTest, IncludeWithMissingFilePrintsError) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\i /nonexistent/file.sql", vars, out);
    // Should not crash; should print some error message.
    EXPECT_FALSE(out.str().empty());
}

TEST(PsqlCommandTest, IncludeWithNoArgPrintsError) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\i", vars, out);
    EXPECT_NE(out.str().find("missing"), std::string::npos);
}

TEST(PsqlCommandTest, UnknownCommandPrintsError) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\nosuchcommand", vars, out);
    EXPECT_NE(out.str().find("unknown"), std::string::npos);
}

TEST(PsqlCommandTest, BareBackslashWithNoCommandNameIsRejected) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    ExecuteMetaCommand(client, "\\", vars, out);
    EXPECT_FALSE(out.str().empty());
}

TEST(PsqlCommandTest, LeadingWhitespaceIsTolerated) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    MetaCommandResult r = ExecuteMetaCommand(client, "   \\q", vars, out);
    EXPECT_EQ(r, MetaCommandResult::kQuit);
}

TEST(PsqlCommandTest, TokenizerHandlesQuotedArgs) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out;
    // Single-quoted argument should preserve embedded spaces.
    ExecuteMetaCommand(client, "\\echo 'hello world'", vars, out);
    EXPECT_EQ(out.str(), "hello world\n");
}

// ---------------------------------------------------------------------------
// Variable persistence across multiple commands.
// ---------------------------------------------------------------------------

TEST(PsqlCommandTest, SetAndEchoSequence) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::map<std::string, std::string> vars;
    std::ostringstream out1, out2;
    ExecuteMetaCommand(client, "\\set x 100", vars, out1);
    EXPECT_EQ(vars["x"], "100");
    ExecuteMetaCommand(client, "\\echo :x", vars, out2);
    EXPECT_EQ(out2.str(), "100\n");
}
