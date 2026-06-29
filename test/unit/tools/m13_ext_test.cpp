// m13_ext_test.cpp — Unit tests for the M13 psql completionization and admin
// toolchain modules added in Task 15.23.
//
// Coverage map:
//   - psql_print:        OutputFormat parse/format-name, per-format renderers,
//                        CSV/HTML/LaTeX/JSON escaping, truncation.
//   - psql_completion:   SQL keyword, meta-command, pset-option completion,
//                        common-prefix computation.
//   - psql_help:         topic lookup, index printing, \? help.
//   - psql_variables:    set/unset/get, substitution, SQL string quoting,
//                        \set parsing.
//   - psql_prompt:       default prompts, % escapes, variable substitution.
//   - psql_crosstabview: column resolution, render.
//   - psql_large_obj:    API surface (no server-side tests).
//   - pg_ctl:            postmaster.pid read/write/remove, IsServerRunning,
//                        StopModeToSignal.
//   - pg_dump:           BuildDropTable/BuildCreateTable/BuildCopyHeader/
//                        BuildInsertStatement, QuoteIdentifier/QuoteLiteral.
//   - pg_restore:        SplitDumpIntoStatements, IsCopyStatement,
//                        IsDataStatement.
//   - sql_admin:         BuildVacuumSql, BuildReindexSql, BuildClusterSql,
//                        BuildCreateDatabaseSql, BuildDropDatabaseSql,
//                        BuildCreateRoleSql, BuildDropRoleSql.
//   - pg_isready:        ReadyStateToString, ReadyStateToExitCode.
//   - pg_config:         GetPgConfigEntries, FindConfigEntry, PrintConfigEntry,
//                        PrintAllConfigEntries.
#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "tools/pg_config.hpp"
#include "tools/pg_ctl.hpp"
#include "tools/pg_dump.hpp"
#include "tools/pg_isready.hpp"
#include "tools/pg_restore.hpp"
#include "tools/psql_client.hpp"
#include "tools/psql_completion.hpp"
#include "tools/psql_crosstabview.hpp"
#include "tools/psql_help.hpp"
#include "tools/psql_large_obj.hpp"
#include "tools/psql_print.hpp"
#include "tools/psql_prompt.hpp"
#include "tools/psql_variables.hpp"
#include "tools/sql_admin.hpp"

using pgcpp::tools::AdminResult;
using pgcpp::tools::BuildClusterSql;
using pgcpp::tools::BuildCopyHeader;
using pgcpp::tools::BuildCreateDatabaseSql;
using pgcpp::tools::BuildCreateRoleSql;
using pgcpp::tools::BuildCreateTableStatement;
using pgcpp::tools::BuildDropDatabaseSql;
using pgcpp::tools::BuildDropRoleSql;
using pgcpp::tools::BuildDropTableStatement;
using pgcpp::tools::BuildInsertStatement;
using pgcpp::tools::BuildReindexSql;
using pgcpp::tools::BuildVacuumSql;
using pgcpp::tools::CheckServerReady;
using pgcpp::tools::ClusterDatabase;
using pgcpp::tools::ClusterOptions;
using pgcpp::tools::CommonPrefix;
using pgcpp::tools::CompleteLine;
using pgcpp::tools::CompleteMetaCommand;
using pgcpp::tools::CompletePsetOption;
using pgcpp::tools::CompleteSqlCommand;
using pgcpp::tools::CompletionContext;
using pgcpp::tools::CreateDatabase;
using pgcpp::tools::CreatedbOptions;
using pgcpp::tools::CreateRole;
using pgcpp::tools::CreateuserOptions;
using pgcpp::tools::CrosstabOptions;
using pgcpp::tools::CrosstabResult;
using pgcpp::tools::DropDatabase;
using pgcpp::tools::DropRole;
using pgcpp::tools::DumpOptions;
using pgcpp::tools::DumpResult;
using pgcpp::tools::EscapeCsvField;
using pgcpp::tools::EscapeHtml;
using pgcpp::tools::EscapeJson;
using pgcpp::tools::EscapeLatex;
using pgcpp::tools::FindConfigEntry;
using pgcpp::tools::FindSqlHelpTopic;
using pgcpp::tools::FormatName;
using pgcpp::tools::FormatPrompt;
using pgcpp::tools::GetPgConfigEntries;
using pgcpp::tools::GetSqlHelpTopics;
using pgcpp::tools::IsCopyStatement;
using pgcpp::tools::IsDataStatement;
using pgcpp::tools::IsReadyOptions;
using pgcpp::tools::IsServerRunning;
using pgcpp::tools::LargeObjectResult;
using pgcpp::tools::MetaCommands;
using pgcpp::tools::OutputFormat;
using pgcpp::tools::ParseFormatName;
using pgcpp::tools::ParseSetCommand;
using pgcpp::tools::PgConfigEntry;
using pgcpp::tools::PgCtlOptions;
using pgcpp::tools::PgCtlResult;
using pgcpp::tools::PgCtlStopMode;
using pgcpp::tools::PrintAllConfigEntries;
using pgcpp::tools::PrintConfigEntry;
using pgcpp::tools::PrintConfigHelp;
using pgcpp::tools::PrintPsqlHelp;
using pgcpp::tools::PrintSqlHelp;
using pgcpp::tools::PrintSqlHelpIndex;
using pgcpp::tools::PromptContext;
using pgcpp::tools::PromptKind;
using pgcpp::tools::PsqlVariables;
using pgcpp::tools::QuoteIdentifier;
using pgcpp::tools::QuoteLiteral;
using pgcpp::tools::QuoteSqlString;
using pgcpp::tools::ReadyState;
using pgcpp::tools::ReadyStateToExitCode;
using pgcpp::tools::ReadyStateToString;
using pgcpp::tools::ReindexDatabase;
using pgcpp::tools::ReindexOptions;
using pgcpp::tools::RemovePostmasterPid;
using pgcpp::tools::RenderCrosstab;
using pgcpp::tools::ResolveColumnIndex;
using pgcpp::tools::RestoreOptions;
using pgcpp::tools::RestoreResult;
using pgcpp::tools::SqlKeywords;
using pgcpp::tools::StopModeToSignal;
using pgcpp::tools::SubstituteVariables;
using pgcpp::tools::TruncateCell;
using pgcpp::tools::VacuumDatabase;
using pgcpp::tools::VacuumOptions;
using pgcpp::tools::WritePostmasterPid;

// ===========================================================================
// psql_print
// ===========================================================================

TEST(PsqlPrintTest, ParseFormatNameAlignsToEnum) {
    OutputFormat f{};
    ASSERT_TRUE(ParseFormatName("aligned", f));
    EXPECT_EQ(f, OutputFormat::kAligned);
    ASSERT_TRUE(ParseFormatName("unaligned", f));
    EXPECT_EQ(f, OutputFormat::kUnaligned);
    ASSERT_TRUE(ParseFormatName("csv", f));
    EXPECT_EQ(f, OutputFormat::kCsv);
    ASSERT_TRUE(ParseFormatName("json", f));
    EXPECT_EQ(f, OutputFormat::kJson);
    ASSERT_TRUE(ParseFormatName("html", f));
    EXPECT_EQ(f, OutputFormat::kHtml);
    ASSERT_TRUE(ParseFormatName("latex", f));
    EXPECT_EQ(f, OutputFormat::kLatex);
}

TEST(PsqlPrintTest, ParseFormatNameRejectsUnknown) {
    OutputFormat f{};
    EXPECT_FALSE(ParseFormatName("xml", f));
    EXPECT_FALSE(ParseFormatName("", f));
}

TEST(PsqlPrintTest, FormatNameRoundTrip) {
    EXPECT_EQ(FormatName(OutputFormat::kAligned), "aligned");
    EXPECT_EQ(FormatName(OutputFormat::kUnaligned), "unaligned");
    EXPECT_EQ(FormatName(OutputFormat::kCsv), "csv");
    EXPECT_EQ(FormatName(OutputFormat::kJson), "json");
    EXPECT_EQ(FormatName(OutputFormat::kHtml), "html");
    EXPECT_EQ(FormatName(OutputFormat::kLatex), "latex");
}

TEST(PsqlPrintTest, EscapeCsvFieldQuotesWhenNeeded) {
    EXPECT_EQ(EscapeCsvField("plain"), "plain");
    EXPECT_EQ(EscapeCsvField("a,b"), "\"a,b\"");
    EXPECT_EQ(EscapeCsvField("a\"b"), "\"a\"\"b\"");
    EXPECT_EQ(EscapeCsvField("a\nb"), "\"a\nb\"");
    EXPECT_EQ(EscapeCsvField("a\rb"), "\"a\rb\"");
}

TEST(PsqlPrintTest, EscapeCsvFieldDoesNotQuotePlain) {
    EXPECT_EQ(EscapeCsvField("hello"), "hello");
    EXPECT_EQ(EscapeCsvField(""), "");
}

TEST(PsqlPrintTest, EscapeHtmlReplacesSpecial) {
    EXPECT_EQ(EscapeHtml("a<b>c"), "a&lt;b&gt;c");
    EXPECT_EQ(EscapeHtml("a&b"), "a&amp;b");
    EXPECT_EQ(EscapeHtml("\"quote\""), "&quot;quote&quot;");
}

TEST(PsqlPrintTest, EscapeJsonReplacesSpecial) {
    EXPECT_EQ(EscapeJson("a\"b"), "a\\\"b");
    EXPECT_EQ(EscapeJson("a\nb"), "a\\nb");
    EXPECT_EQ(EscapeJson("a\\b"), "a\\\\b");
}

TEST(PsqlPrintTest, EscapeLatexReplacesSpecial) {
    EXPECT_NE(EscapeLatex("a_b").find("\\_"), std::string::npos);
    EXPECT_NE(EscapeLatex("a%b").find("\\%"), std::string::npos);
}

TEST(PsqlPrintTest, TruncateCellNoLimitReturnsOriginal) {
    EXPECT_EQ(TruncateCell("hello", 0), "hello");
}

TEST(PsqlPrintTest, TruncateCellShortReturnsOriginal) {
    EXPECT_EQ(TruncateCell("hi", 10), "hi");
}

TEST(PsqlPrintTest, TruncateCellLongAppendsEllipsis) {
    std::string r = TruncateCell("abcdefghij", 5);
    EXPECT_EQ(r.size(), 5u);
    EXPECT_NE(r.find("..."), std::string::npos);
}

TEST(PsqlPrintTest, PrintAlignedContainsHeaderAndFooter) {
    pgcpp::tools::QueryResult r;
    r.success = true;
    r.column_names = {"id", "name"};
    r.rows = {{"1", "alice"}, {"2", "bob"}};
    r.command_tag = "SELECT 2";
    std::ostringstream out;
    pgcpp::tools::PrintAligned(r, {}, out);
    std::string s = out.str();
    EXPECT_NE(s.find("id"), std::string::npos);
    EXPECT_NE(s.find("name"), std::string::npos);
    EXPECT_NE(s.find("alice"), std::string::npos);
    EXPECT_NE(s.find("(2 rows)"), std::string::npos);
}

TEST(PsqlPrintTest, PrintUnalignedUsesPipeSeparator) {
    pgcpp::tools::QueryResult r;
    r.success = true;
    r.column_names = {"a", "b"};
    r.rows = {{"1", "2"}};
    std::ostringstream out;
    pgcpp::tools::PrintUnaligned(r, {}, out);
    std::string s = out.str();
    EXPECT_NE(s.find("1|2"), std::string::npos);
}

TEST(PsqlPrintTest, PrintCsvProducesQuotedFields) {
    pgcpp::tools::QueryResult r;
    r.success = true;
    r.column_names = {"a", "b"};
    r.rows = {{"1", "hello,world"}};
    std::ostringstream out;
    pgcpp::tools::PrintCsv(r, out);
    std::string s = out.str();
    EXPECT_NE(s.find("\"hello,world\""), std::string::npos);
}

TEST(PsqlPrintTest, PrintJsonProducesArray) {
    pgcpp::tools::QueryResult r;
    r.success = true;
    r.column_names = {"a", "b"};
    r.rows = {{"1", "x"}};
    std::ostringstream out;
    pgcpp::tools::PrintJson(r, out);
    std::string s = out.str();
    EXPECT_NE(s.find("["), std::string::npos);
    EXPECT_NE(s.find("{"), std::string::npos);
    EXPECT_NE(s.find("\"a\""), std::string::npos);
    EXPECT_NE(s.find("1"), std::string::npos);
}

TEST(PsqlPrintTest, PrintHtmlProducesTable) {
    pgcpp::tools::QueryResult r;
    r.success = true;
    r.column_names = {"a"};
    r.rows = {{"1"}};
    std::ostringstream out;
    pgcpp::tools::PrintHtml(r, out);
    std::string s = out.str();
    EXPECT_NE(s.find("<table"), std::string::npos);
    EXPECT_NE(s.find("<td"), std::string::npos);
}

TEST(PsqlPrintTest, PrintLatexProducesTabular) {
    pgcpp::tools::QueryResult r;
    r.success = true;
    r.column_names = {"a"};
    r.rows = {{"1"}};
    std::ostringstream out;
    pgcpp::tools::PrintLatex(r, out);
    std::string s = out.str();
    EXPECT_NE(s.find("tabular"), std::string::npos);
}

TEST(PsqlPrintTest, PrintQueryResultDispatchesByFormat) {
    pgcpp::tools::QueryResult r;
    r.success = true;
    r.column_names = {"a"};
    r.rows = {{"1"}};
    r.command_tag = "SELECT 1";
    pgcpp::tools::PrintOptions opts;
    opts.format = OutputFormat::kCsv;
    std::ostringstream out;
    int n = pgcpp::tools::PrintQueryResult(r, opts, out);
    EXPECT_EQ(n, 1);
    EXPECT_FALSE(out.str().empty());
}

TEST(PsqlPrintTest, PrintQueryResultHandlesError) {
    pgcpp::tools::QueryResult r;
    r.success = false;
    r.error_message = "boom";
    std::ostringstream out;
    pgcpp::tools::PrintQueryResult(r, {}, out);
    EXPECT_NE(out.str().find("boom"), std::string::npos);
}

// ===========================================================================
// psql_completion
// ===========================================================================

TEST(PsqlCompletionTest, SqlKeywordsNonEmpty) {
    const auto& kws = SqlKeywords();
    EXPECT_FALSE(kws.empty());
    // A few fundamental keywords should always be present.
    bool has_select = false, has_from = false, has_where = false;
    for (const auto& k : kws) {
        if (k == "SELECT")
            has_select = true;
        if (k == "FROM")
            has_from = true;
        if (k == "WHERE")
            has_where = true;
    }
    EXPECT_TRUE(has_select);
    EXPECT_TRUE(has_from);
    EXPECT_TRUE(has_where);
}

TEST(PsqlCompletionTest, MetaCommandsNonEmpty) {
    const auto& cmds = MetaCommands();
    EXPECT_FALSE(cmds.empty());
    bool has_q = false, has_dt = false;
    for (const auto& c : cmds) {
        if (c == "\\q")
            has_q = true;
        if (c == "\\dt")
            has_dt = true;
    }
    EXPECT_TRUE(has_q);
    EXPECT_TRUE(has_dt);
}

TEST(PsqlCompletionTest, CommonPrefixBasic) {
    EXPECT_EQ(CommonPrefix({"abc", "abd", "abe"}), "ab");
    EXPECT_EQ(CommonPrefix({"foo", "bar"}), "");
    EXPECT_EQ(CommonPrefix({}), "");
    EXPECT_EQ(CommonPrefix({"only"}), "only");
}

TEST(PsqlCompletionTest, CompleteMetaCommandSuggestsCommands) {
    auto r = CompleteMetaCommand("\\d");
    EXPECT_FALSE(r.candidates.empty());
    // Should suggest \dt, \dv, \du, \d, etc.
    bool has_dt = false;
    for (const auto& c : r.candidates) {
        if (c == "\\dt")
            has_dt = true;
    }
    EXPECT_TRUE(has_dt);
}

TEST(PsqlCompletionTest, CompleteMetaCommandExactMatch) {
    auto r = CompleteMetaCommand("\\q");
    ASSERT_FALSE(r.candidates.empty());
    bool has_q = false;
    for (const auto& c : r.candidates) {
        if (c == "\\q")
            has_q = true;
    }
    EXPECT_TRUE(has_q);
}

TEST(PsqlCompletionTest, CompletePsetOptionSuggestsOptions) {
    auto r = CompletePsetOption("for");
    EXPECT_FALSE(r.candidates.empty());
}

TEST(PsqlCompletionTest, CompleteSqlCommandSuggestsKeywords) {
    CompletionContext ctx;
    ctx.line = "SEL";
    ctx.cursor = 3;
    auto r = CompleteSqlCommand(ctx);
    EXPECT_FALSE(r.candidates.empty());
    bool has_select = false;
    for (const auto& c : r.candidates) {
        if (c == "SELECT")
            has_select = true;
    }
    EXPECT_TRUE(has_select);
}

TEST(PsqlCompletionTest, CompleteLineDispatchesSql) {
    CompletionContext ctx;
    ctx.line = "SEL";
    ctx.cursor = 3;
    auto r = CompleteLine(ctx);
    EXPECT_FALSE(r.candidates.empty());
}

TEST(PsqlCompletionTest, CompleteLineDispatchesMeta) {
    CompletionContext ctx;
    ctx.line = "\\d";
    ctx.cursor = 2;
    auto r = CompleteLine(ctx);
    EXPECT_FALSE(r.candidates.empty());
}

// ===========================================================================
// psql_help
// ===========================================================================

TEST(PsqlHelpTest, GetTopicsNonEmpty) {
    const auto& topics = GetSqlHelpTopics();
    EXPECT_FALSE(topics.empty());
}

TEST(PsqlHelpTest, FindSelectTopic) {
    const auto* t = FindSqlHelpTopic("SELECT");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->name, "SELECT");
    EXPECT_FALSE(t->syntax.empty());
}

TEST(PsqlHelpTest, FindIsCaseInsensitive) {
    const auto* t = FindSqlHelpTopic("select");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->name, "SELECT");
}

TEST(PsqlHelpTest, FindUnknownReturnsNull) {
    EXPECT_EQ(FindSqlHelpTopic("NOSUCHCOMMAND"), nullptr);
}

TEST(PsqlHelpTest, PrintSqlHelpPrintsKnownTopic) {
    std::ostringstream out;
    bool found = PrintSqlHelp("INSERT", out);
    EXPECT_TRUE(found);
    EXPECT_NE(out.str().find("INSERT"), std::string::npos);
}

TEST(PsqlHelpTest, PrintSqlHelpReturnsFalseForUnknown) {
    std::ostringstream out;
    bool found = PrintSqlHelp("NOSUCH", out);
    EXPECT_FALSE(found);
}

TEST(PsqlHelpTest, PrintSqlHelpIndexListsTopics) {
    std::ostringstream out;
    PrintSqlHelpIndex(out);
    EXPECT_FALSE(out.str().empty());
    EXPECT_NE(out.str().find("SELECT"), std::string::npos);
}

TEST(PsqlHelpTest, PrintPsqlHelpListsBackslashCommands) {
    std::ostringstream out;
    PrintPsqlHelp(out);
    EXPECT_FALSE(out.str().empty());
    EXPECT_NE(out.str().find("\\q"), std::string::npos);
}

// ===========================================================================
// psql_variables
// ===========================================================================

TEST(PsqlVariablesTest, DefaultsArePopulated) {
    PsqlVariables v;
    EXPECT_EQ(v.Get("AUTOCOMMIT"), "on");
    EXPECT_FALSE(v.Get("PROMPT1").empty());
}

TEST(PsqlVariablesTest, SetAndGet) {
    PsqlVariables v;
    v.Set("foo", "bar");
    EXPECT_EQ(v.Get("foo"), "bar");
    EXPECT_TRUE(v.IsSet("foo"));
}

TEST(PsqlVariablesTest, UnsetRemovesVariable) {
    PsqlVariables v;
    v.Set("foo", "bar");
    v.Unset("foo");
    EXPECT_FALSE(v.IsSet("foo"));
    EXPECT_EQ(v.Get("foo"), "");
}

TEST(PsqlVariablesTest, UnsetNonExistentIsNoOp) {
    PsqlVariables v;
    v.Unset("never_set");
    EXPECT_FALSE(v.IsSet("never_set"));
}

TEST(PsqlVariablesTest, AllReturnsMap) {
    PsqlVariables v;
    v.Set("foo", "bar");
    const auto& m = v.All();
    EXPECT_GT(m.size(), 0u);
    EXPECT_EQ(m.at("foo"), "bar");
}

TEST(PsqlVariablesTest, SubstituteVariablesReplacesName) {
    PsqlVariables v;
    v.Set("foo", "bar");
    EXPECT_EQ(SubstituteVariables(":foo", v), "bar");
}

TEST(PsqlVariablesTest, SubstituteVariablesLeavesUnknownAsIs) {
    PsqlVariables v;
    EXPECT_EQ(SubstituteVariables(":nope", v), ":nope");
}

TEST(PsqlVariablesTest, SubstituteVariablesQuotedForm) {
    PsqlVariables v;
    v.Set("foo", "it's me");
    EXPECT_EQ(SubstituteVariables(":'foo'", v), "'it''s me'");
}

TEST(PsqlVariablesTest, QuoteSqlStringDoublesQuotes) {
    EXPECT_EQ(QuoteSqlString("plain"), "'plain'");
    EXPECT_EQ(QuoteSqlString("it's"), "'it''s'");
    EXPECT_EQ(QuoteSqlString(""), "''");
}

TEST(PsqlVariablesTest, ParseSetCommandNameAndValue) {
    std::string name, value;
    ASSERT_TRUE(ParseSetCommand("myvar hello world", name, value));
    EXPECT_EQ(name, "myvar");
    EXPECT_EQ(value, "hello world");
}

TEST(PsqlVariablesTest, ParseSetCommandNoValue) {
    std::string name, value;
    ASSERT_TRUE(ParseSetCommand("myvar", name, value));
    EXPECT_EQ(name, "myvar");
    EXPECT_EQ(value, "");
}

TEST(PsqlVariablesTest, ParseSetCommandNoNameReturnsFalse) {
    std::string name, value;
    EXPECT_FALSE(ParseSetCommand("", name, value));
    EXPECT_FALSE(ParseSetCommand("   ", name, value));
}

// ===========================================================================
// psql_prompt
// ===========================================================================

TEST(PsqlPromptTest, DefaultPromptsAreNonEmpty) {
    EXPECT_FALSE(pgcpp::tools::DefaultPrompt(PromptKind::kPrompt1).empty());
    EXPECT_FALSE(pgcpp::tools::DefaultPrompt(PromptKind::kPrompt2).empty());
    EXPECT_FALSE(pgcpp::tools::DefaultPrompt(PromptKind::kPrompt3).empty());
}

TEST(PsqlPromptTest, FormatPromptLiteral) {
    PsqlVariables vars;
    PromptContext ctx;
    EXPECT_EQ(FormatPrompt("hello", ctx, vars), "hello");
}

TEST(PsqlPromptTest, FormatPromptPercentPercent) {
    PsqlVariables vars;
    PromptContext ctx;
    EXPECT_EQ(FormatPrompt("%%", ctx, vars), "%");
}

TEST(PsqlPromptTest, FormatPromptDatabaseSubstitution) {
    PsqlVariables vars;
    PromptContext ctx;
    ctx.database = "mydb";
    EXPECT_EQ(FormatPrompt("%/", ctx, vars), "mydb");
}

TEST(PsqlPromptTest, FormatPromptUserSubstitution) {
    PsqlVariables vars;
    PromptContext ctx;
    ctx.user = "alice";
    EXPECT_EQ(FormatPrompt("%n", ctx, vars), "alice");
}

TEST(PsqlPromptTest, FormatPromptPortSubstitution) {
    PsqlVariables vars;
    PromptContext ctx;
    ctx.port = 5433;
    EXPECT_EQ(FormatPrompt("%>", ctx, vars), "5433");
}

TEST(PsqlPromptTest, FormatPromptSuperuserMark) {
    PsqlVariables vars;
    PromptContext ctx;
    ctx.is_superuser = true;
    EXPECT_EQ(FormatPrompt("%#", ctx, vars), "#");
    ctx.is_superuser = false;
    EXPECT_EQ(FormatPrompt("%#", ctx, vars), ">");
}

TEST(PsqlPromptTest, FormatPromptLastStatus) {
    PsqlVariables vars;
    PromptContext ctx;
    ctx.last_status = 42;
    EXPECT_EQ(FormatPrompt("%?", ctx, vars), "42");
}

TEST(PsqlPromptTest, FormatPromptVariableSubstitution) {
    PsqlVariables vars;
    vars.Set("myvar", "hello");
    PromptContext ctx;
    EXPECT_EQ(FormatPrompt(":myvar", ctx, vars), "hello");
}

// ===========================================================================
// psql_crosstabview
// ===========================================================================

TEST(PsqlCrosstabviewTest, ResolveColumnIndexByName) {
    pgcpp::tools::QueryResult r;
    r.column_names = {"year", "region", "sales"};
    EXPECT_EQ(ResolveColumnIndex(r, "region", 2), 1);
    EXPECT_EQ(ResolveColumnIndex(r, "sales", 3), 2);
}

TEST(PsqlCrosstabviewTest, ResolveColumnIndexByFallback) {
    pgcpp::tools::QueryResult r;
    r.column_names = {"year", "region", "sales"};
    EXPECT_EQ(ResolveColumnIndex(r, "", 1), 0);
    EXPECT_EQ(ResolveColumnIndex(r, "", 3), 2);
}

TEST(PsqlCrosstabviewTest, ResolveColumnIndexInvalidName) {
    pgcpp::tools::QueryResult r;
    r.column_names = {"year"};
    EXPECT_EQ(ResolveColumnIndex(r, "nope", 1), -1);
}

TEST(PsqlCrosstabviewTest, RenderCrosstabNotEnoughColumns) {
    pgcpp::tools::QueryResult r;
    r.column_names = {"a", "b"};
    r.rows = {{"1", "2"}};
    std::ostringstream out;
    EXPECT_EQ(RenderCrosstab(r, {}, out), CrosstabResult::kNotEnoughColumns);
}

TEST(PsqlCrosstabviewTest, RenderCrosstabOk) {
    pgcpp::tools::QueryResult r;
    r.success = true;
    r.column_names = {"year", "region", "sales"};
    r.rows = {
        {"2020", "north", "100"},
        {"2020", "south", "120"},
        {"2021", "north", "105"},
    };
    CrosstabOptions opts;
    std::ostringstream out;
    EXPECT_EQ(RenderCrosstab(r, opts, out), CrosstabResult::kOk);
    std::string s = out.str();
    EXPECT_NE(s.find("north"), std::string::npos);
    EXPECT_NE(s.find("south"), std::string::npos);
    EXPECT_NE(s.find("2020"), std::string::npos);
}

// ===========================================================================
// psql_large_obj (API surface only — no server)
// ===========================================================================

TEST(PsqlLargeObjTest, LoListWithoutServerFailsCleanly) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    std::ostringstream out;
    LargeObjectResult r = pgcpp::tools::lo_list(client, out);
    // Without a server connection this should not return kOk.
    EXPECT_NE(r, LargeObjectResult::kOk);
}

TEST(PsqlLargeObjTest, LoUnlinkWithoutServerFailsCleanly) {
    pgcpp::tools::PsqlClient client("127.0.0.1", 0);
    LargeObjectResult r = pgcpp::tools::lo_unlink(client, 12345);
    EXPECT_NE(r, LargeObjectResult::kOk);
}

// ===========================================================================
// pg_ctl
// ===========================================================================

TEST(PgCtlTest, StopModeToSignalMapsCorrectly) {
    EXPECT_EQ(StopModeToSignal(PgCtlStopMode::kSmart), SIGTERM);
    EXPECT_EQ(StopModeToSignal(PgCtlStopMode::kFast), SIGINT);
    EXPECT_EQ(StopModeToSignal(PgCtlStopMode::kImmediate), SIGQUIT);
}

TEST(PgCtlTest, WriteAndReadPostmasterPid) {
    std::string dir = "/tmp/pgcpp_pgctl_test_" + std::to_string(getpid());
    ASSERT_EQ(system(("rm -rf " + dir).c_str()), 0);
    ASSERT_EQ(mkdir(dir.c_str(), 0700), 0);

    EXPECT_TRUE(WritePostmasterPid(dir, 4242));
    EXPECT_EQ(pgcpp::tools::ReadPostmasterPid(dir), 4242);
    // IsServerRunning checks kill(pid, 0); PID 4242 is not a live process,
    // so this returns false (the file is correctly written but the process
    // doesn't exist).
    EXPECT_FALSE(IsServerRunning(dir));

    RemovePostmasterPid(dir);
    EXPECT_FALSE(IsServerRunning(dir));
    EXPECT_EQ(pgcpp::tools::ReadPostmasterPid(dir), 0);

    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgCtlTest, ReadPostmasterPidMissingFileReturnsZero) {
    std::string dir = "/tmp/pgcpp_pgctl_missing_" + std::to_string(getpid());
    ASSERT_EQ(system(("rm -rf " + dir).c_str()), 0);
    EXPECT_EQ(pgcpp::tools::ReadPostmasterPid(dir), 0);
    EXPECT_FALSE(IsServerRunning(dir));
}

TEST(PgCtlTest, ReadPostmasterPidMalformedReturnsZero) {
    std::string dir = "/tmp/pgcpp_pgctl_malformed_" + std::to_string(getpid());
    ASSERT_EQ(system(("rm -rf " + dir + " && mkdir -p " + dir).c_str()), 0);
    std::string path = dir + "/postmaster.pid";
    FILE* f = fopen(path.c_str(), "w");
    ASSERT_NE(f, nullptr);
    fputs("not_a_number\n", f);
    fclose(f);
    EXPECT_EQ(pgcpp::tools::ReadPostmasterPid(dir), 0);
    EXPECT_EQ(system(("rm -rf " + dir).c_str()), 0);
}

TEST(PgCtlTest, PgCtlMainStatusOnMissingDir) {
    PgCtlOptions opts;
    opts.data_dir = "/tmp/pgcpp_pgctl_nonexistent_" + std::to_string(getpid());
    opts.action = pgcpp::tools::PgCtlAction::kStatus;
    // Implementation returns kNoPostmasterPid (not kInvalidDataDir) when the
    // directory exists but no postmaster.pid is found. kInvalidDataDir is
    // reserved for an empty data_dir.
    EXPECT_EQ(pgcpp::tools::PgCtlMain(opts), PgCtlResult::kNoPostmasterPid);
}

TEST(PgCtlTest, PgCtlMainEmptyDataDirIsInvalid) {
    PgCtlOptions opts;
    opts.data_dir = "";
    opts.action = pgcpp::tools::PgCtlAction::kStatus;
    EXPECT_EQ(pgcpp::tools::PgCtlMain(opts), PgCtlResult::kInvalidDataDir);
}

// ===========================================================================
// pg_dump (helpers only — full dump requires a server)
// ===========================================================================

TEST(PgDumpTest, QuoteIdentifierDoublesQuotes) {
    EXPECT_EQ(QuoteIdentifier("foo"), "\"foo\"");
    EXPECT_EQ(QuoteIdentifier("a\"b"), "\"a\"\"b\"");
    EXPECT_EQ(QuoteIdentifier(""), "\"\"");
}

TEST(PgDumpTest, QuoteLiteralDoublesQuotes) {
    EXPECT_EQ(QuoteLiteral("foo"), "'foo'");
    EXPECT_EQ(QuoteLiteral("it's"), "'it''s'");
    EXPECT_EQ(QuoteLiteral(""), "''");
}

TEST(PgDumpTest, BuildDropTableStatement) {
    // Identifier is double-quoted and statement ends with newline.
    EXPECT_EQ(BuildDropTableStatement("foo"), "DROP TABLE IF EXISTS \"foo\";\n");
}

TEST(PgDumpTest, BuildCreateTableStatement) {
    std::vector<std::pair<std::string, std::string>> cols = {
        {"id", "INTEGER"},
        {"name", "TEXT"},
    };
    std::string s = BuildCreateTableStatement("mytable", cols);
    EXPECT_NE(s.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(s.find("mytable"), std::string::npos);
    EXPECT_NE(s.find("id"), std::string::npos);
    EXPECT_NE(s.find("INTEGER"), std::string::npos);
    EXPECT_NE(s.find("name"), std::string::npos);
    EXPECT_NE(s.find("TEXT"), std::string::npos);
}

TEST(PgDumpTest, BuildCopyHeader) {
    std::vector<std::string> cols = {"a", "b"};
    std::string s = BuildCopyHeader("mytable", cols);
    EXPECT_NE(s.find("COPY"), std::string::npos);
    EXPECT_NE(s.find("mytable"), std::string::npos);
    EXPECT_NE(s.find("a"), std::string::npos);
    EXPECT_NE(s.find("FROM stdin"), std::string::npos);
}

TEST(PgDumpTest, BuildInsertStatement) {
    std::vector<std::string> cols = {"a", "b"};
    std::vector<std::string> vals = {"1", "2"};
    std::string s = BuildInsertStatement("t", cols, vals);
    EXPECT_NE(s.find("INSERT INTO"), std::string::npos);
    EXPECT_NE(s.find("t"), std::string::npos);
    EXPECT_NE(s.find("1"), std::string::npos);
    EXPECT_NE(s.find("2"), std::string::npos);
}

// ===========================================================================
// pg_restore (statement splitting)
// ===========================================================================

TEST(PgRestoreTest, SplitDumpSimpleSemicolons) {
    auto stmts = pgcpp::tools::SplitDumpIntoStatements("SELECT 1; SELECT 2;");
    ASSERT_EQ(stmts.size(), 2u);
    EXPECT_NE(stmts[0].find("SELECT 1"), std::string::npos);
    EXPECT_NE(stmts[1].find("SELECT 2"), std::string::npos);
}

TEST(PgRestoreTest, SplitDumpIgnoresSemicolonsInStringLiterals) {
    auto stmts = pgcpp::tools::SplitDumpIntoStatements("SELECT 'a;b'; SELECT 'c;d';");
    ASSERT_EQ(stmts.size(), 2u);
}

TEST(PgRestoreTest, SplitDumpHandlesDollarQuoting) {
    std::string dump =
        "CREATE FUNCTION f() RETURNS int AS $$ BEGIN\n"
        "  RETURN 1;\n"
        "END; $$ LANGUAGE plpgsql;\n"
        "SELECT 1;";
    auto stmts = pgcpp::tools::SplitDumpIntoStatements(dump);
    // Should split into exactly 2 statements (function body stays intact).
    ASSERT_EQ(stmts.size(), 2u);
}

TEST(PgRestoreTest, SplitDumpHandlesCopyBlock) {
    std::string dump =
        "COPY t (a, b) FROM stdin;\n"
        "1\t2\n"
        "3\t4\n"
        "\\.\n"
        "SELECT 1;";
    auto stmts = pgcpp::tools::SplitDumpIntoStatements(dump);
    // The COPY block is a single statement (including data + terminator).
    ASSERT_GE(stmts.size(), 2u);
    EXPECT_TRUE(IsCopyStatement(stmts[0]));
}

TEST(PgRestoreTest, IsCopyStatementDetectsCopy) {
    EXPECT_TRUE(IsCopyStatement("COPY t (a) FROM stdin;"));
    EXPECT_FALSE(IsCopyStatement("SELECT 1;"));
    EXPECT_FALSE(IsCopyStatement("INSERT INTO t VALUES (1);"));
}

TEST(PgRestoreTest, IsDataStatementDetectsDataAndInsert) {
    EXPECT_TRUE(IsDataStatement("COPY t (a) FROM stdin;"));
    EXPECT_TRUE(IsDataStatement("INSERT INTO t VALUES (1);"));
    EXPECT_FALSE(IsDataStatement("SELECT 1;"));
    EXPECT_FALSE(IsDataStatement("CREATE TABLE t (a int);"));
}

// ===========================================================================
// sql_admin (Build*Sql helpers)
// ===========================================================================

TEST(SqlAdminTest, BuildVacuumSqlBasic) {
    VacuumOptions opts;
    EXPECT_EQ(BuildVacuumSql(opts), "VACUUM;");
}

TEST(SqlAdminTest, BuildVacuumSqlFullAnalyzeVerbose) {
    VacuumOptions opts;
    opts.full = true;
    opts.analyze = true;
    opts.verbose = true;
    std::string s = BuildVacuumSql(opts);
    EXPECT_NE(s.find("VACUUM FULL"), std::string::npos);
    EXPECT_NE(s.find("ANALYZE"), std::string::npos);
    EXPECT_NE(s.find("VERBOSE"), std::string::npos);
}

TEST(SqlAdminTest, BuildVacuumSqlWithTable) {
    VacuumOptions opts;
    opts.table = "mytable";
    std::string s = BuildVacuumSql(opts);
    EXPECT_NE(s.find("mytable"), std::string::npos);
}

TEST(SqlAdminTest, BuildVacuumSqlFreezeSkipLocked) {
    VacuumOptions opts;
    opts.freeze = true;
    opts.skip_locked = true;
    std::string s = BuildVacuumSql(opts);
    EXPECT_NE(s.find("FREEZE"), std::string::npos);
    // Implementation emits SKIP_LOCKED (with underscore, matching PG's syntax).
    EXPECT_NE(s.find("SKIP_LOCKED"), std::string::npos);
}

TEST(SqlAdminTest, BuildReindexSqlDatabase) {
    ReindexOptions opts;
    opts.kind = ReindexOptions::Kind::kDatabase;
    opts.name = "mydb";
    std::string s = BuildReindexSql(opts);
    EXPECT_NE(s.find("REINDEX DATABASE"), std::string::npos);
    EXPECT_NE(s.find("mydb"), std::string::npos);
}

TEST(SqlAdminTest, BuildReindexSqlTable) {
    ReindexOptions opts;
    opts.kind = ReindexOptions::Kind::kTable;
    opts.name = "mytable";
    std::string s = BuildReindexSql(opts);
    EXPECT_NE(s.find("REINDEX TABLE"), std::string::npos);
}

TEST(SqlAdminTest, BuildReindexSqlIndexConcurrently) {
    ReindexOptions opts;
    opts.kind = ReindexOptions::Kind::kIndex;
    opts.name = "myidx";
    opts.concurrently = true;
    std::string s = BuildReindexSql(opts);
    EXPECT_NE(s.find("CONCURRENTLY"), std::string::npos);
    EXPECT_NE(s.find("INDEX"), std::string::npos);
}

TEST(SqlAdminTest, BuildReindexSqlSystem) {
    ReindexOptions opts;
    opts.kind = ReindexOptions::Kind::kSystem;
    opts.name = "mydb";
    std::string s = BuildReindexSql(opts);
    EXPECT_NE(s.find("SYSTEM"), std::string::npos);
}

TEST(SqlAdminTest, BuildClusterSqlBasic) {
    ClusterOptions opts;
    std::string s = BuildClusterSql(opts);
    EXPECT_NE(s.find("CLUSTER"), std::string::npos);
}

TEST(SqlAdminTest, BuildClusterSqlWithTableAndIndex) {
    ClusterOptions opts;
    opts.table = "t";
    opts.index = "i";
    std::string s = BuildClusterSql(opts);
    EXPECT_NE(s.find("t"), std::string::npos);
    EXPECT_NE(s.find("USING"), std::string::npos);
    EXPECT_NE(s.find("i"), std::string::npos);
}

TEST(SqlAdminTest, BuildClusterSqlVerbose) {
    ClusterOptions opts;
    opts.verbose = true;
    std::string s = BuildClusterSql(opts);
    EXPECT_NE(s.find("VERBOSE"), std::string::npos);
}

TEST(SqlAdminTest, BuildCreateDatabaseSqlBasic) {
    CreatedbOptions opts;
    opts.name = "newdb";
    std::string s = BuildCreateDatabaseSql(opts);
    EXPECT_NE(s.find("CREATE DATABASE"), std::string::npos);
    EXPECT_NE(s.find("newdb"), std::string::npos);
}

TEST(SqlAdminTest, BuildCreateDatabaseSqlWithOwnerAndTemplate) {
    CreatedbOptions opts;
    opts.name = "newdb";
    opts.owner = "alice";
    opts.template_db = "template0";
    std::string s = BuildCreateDatabaseSql(opts);
    EXPECT_NE(s.find("OWNER"), std::string::npos);
    EXPECT_NE(s.find("TEMPLATE"), std::string::npos);
    EXPECT_NE(s.find("alice"), std::string::npos);
    EXPECT_NE(s.find("template0"), std::string::npos);
}

TEST(SqlAdminTest, BuildDropDatabaseSqlBasic) {
    // Identifier is double-quoted per QuoteIdentifier.
    EXPECT_EQ(BuildDropDatabaseSql("mydb", false), "DROP DATABASE \"mydb\";");
}

TEST(SqlAdminTest, BuildDropDatabaseSqlIfExists) {
    std::string s = BuildDropDatabaseSql("mydb", true);
    EXPECT_NE(s.find("IF EXISTS"), std::string::npos);
}

TEST(SqlAdminTest, BuildCreateRoleSqlBasic) {
    CreateuserOptions opts;
    opts.name = "alice";
    std::string s = BuildCreateRoleSql(opts);
    EXPECT_NE(s.find("CREATE ROLE"), std::string::npos);
    EXPECT_NE(s.find("alice"), std::string::npos);
}

TEST(SqlAdminTest, BuildCreateRoleSqlSuperuserLogin) {
    CreateuserOptions opts;
    opts.name = "alice";
    opts.superuser = true;
    opts.login = true;
    std::string s = BuildCreateRoleSql(opts);
    EXPECT_NE(s.find("SUPERUSER"), std::string::npos);
    EXPECT_NE(s.find("LOGIN"), std::string::npos);
}

TEST(SqlAdminTest, BuildCreateRoleSqlWithPassword) {
    CreateuserOptions opts;
    opts.name = "alice";
    opts.password = "secret";
    std::string s = BuildCreateRoleSql(opts);
    EXPECT_NE(s.find("PASSWORD"), std::string::npos);
    EXPECT_NE(s.find("secret"), std::string::npos);
}

TEST(SqlAdminTest, BuildDropRoleSqlBasic) {
    // Identifier is double-quoted per QuoteIdentifier.
    EXPECT_EQ(BuildDropRoleSql("alice", false), "DROP ROLE \"alice\";");
}

TEST(SqlAdminTest, BuildDropRoleSqlIfExists) {
    std::string s = BuildDropRoleSql("alice", true);
    EXPECT_NE(s.find("IF EXISTS"), std::string::npos);
}

TEST(SqlAdminTest, AdminFunctionsFailWithoutServer) {
    VacuumOptions vopts;
    EXPECT_NE(VacuumDatabase("127.0.0.1", 0, "x", vopts), AdminResult::kOk);
    ReindexOptions ropts;
    EXPECT_NE(ReindexDatabase("127.0.0.1", 0, "x", ropts), AdminResult::kOk);
    ClusterOptions copts;
    EXPECT_NE(ClusterDatabase("127.0.0.1", 0, "x", copts), AdminResult::kOk);
    CreatedbOptions dbopts;
    dbopts.name = "x";
    EXPECT_NE(CreateDatabase("127.0.0.1", 0, "x", dbopts), AdminResult::kOk);
    EXPECT_NE(DropDatabase("127.0.0.1", 0, "x", "x", false), AdminResult::kOk);
    CreateuserOptions uopts;
    uopts.name = "x";
    EXPECT_NE(CreateRole("127.0.0.1", 0, "x", uopts), AdminResult::kOk);
    EXPECT_NE(DropRole("127.0.0.1", 0, "x", "x", false), AdminResult::kOk);
}

// ===========================================================================
// pg_isready
// ===========================================================================

TEST(PgIsreadyTest, ReadyStateToStringAccepting) {
    EXPECT_STREQ(ReadyStateToString(ReadyState::kAccepting), "accepting connections");
}

TEST(PgIsreadyTest, ReadyStateToStringRejecting) {
    EXPECT_NE(std::string(ReadyStateToString(ReadyState::kRejecting)), "");
}

TEST(PgIsreadyTest, ReadyStateToStringNoResponse) {
    EXPECT_NE(std::string(ReadyStateToString(ReadyState::kNoResponse)), "");
}

TEST(PgIsreadyTest, ReadyStateToStringNoAttempt) {
    EXPECT_NE(std::string(ReadyStateToString(ReadyState::kNoAttempt)), "");
}

TEST(PgIsreadyTest, ReadyStateToExitCode) {
    EXPECT_EQ(ReadyStateToExitCode(ReadyState::kAccepting), 0);
    EXPECT_EQ(ReadyStateToExitCode(ReadyState::kRejecting), 1);
    EXPECT_EQ(ReadyStateToExitCode(ReadyState::kNoResponse), 2);
    EXPECT_EQ(ReadyStateToExitCode(ReadyState::kNoAttempt), 3);
}

TEST(PgIsreadyTest, CheckServerReadyNoResponseOnPort0) {
    IsReadyOptions opts;
    opts.port = 0;  // invalid port — should not connect
    ReadyState state = CheckServerReady(opts);
    // Without a server on port 0 we should get either kNoResponse or
    // kNoAttempt (an implementation detail). Either way, not kAccepting.
    EXPECT_NE(state, ReadyState::kAccepting);
}

// ===========================================================================
// pg_config
// ===========================================================================

TEST(PgConfigTest, GetEntriesNonEmpty) {
    const auto& entries = GetPgConfigEntries();
    EXPECT_FALSE(entries.empty());
}

TEST(PgConfigTest, FindKnownEntry) {
    // --version should always be present.
    const auto* e = FindConfigEntry("--version");
    ASSERT_NE(e, nullptr);
    EXPECT_FALSE(e->value.empty());
}

TEST(PgConfigTest, FindUnknownEntryReturnsNull) {
    EXPECT_EQ(FindConfigEntry("--nosuchoption"), nullptr);
}

TEST(PgConfigTest, PrintConfigEntryKnown) {
    std::ostringstream out;
    bool found = PrintConfigEntry("--version", out);
    EXPECT_TRUE(found);
    EXPECT_FALSE(out.str().empty());
}

TEST(PgConfigTest, PrintConfigEntryUnknownReturnsFalse) {
    std::ostringstream out;
    bool found = PrintConfigEntry("--nosuchoption", out);
    EXPECT_FALSE(found);
}

TEST(PgConfigTest, PrintAllConfigEntries) {
    std::ostringstream out;
    PrintAllConfigEntries(out);
    std::string s = out.str();
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("--"), std::string::npos);
}

TEST(PgConfigTest, PrintConfigHelpIsNotEmpty) {
    std::ostringstream out;
    PrintConfigHelp(out);
    EXPECT_FALSE(out.str().empty());
}
