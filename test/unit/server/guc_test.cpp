// guc_test.cpp — Unit tests for GUC (postgresql.conf) loading (M12 Phase 15.10.1).
//
// Tests parsing of postgresql.conf-style configuration files into a GucConfig
// and application of those values to a ServerConfig.

#include "server/guc.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "server/main.hpp"
#include "server/postmaster.hpp"

using pgcpp::server::GucConfig;
using pgcpp::server::LoadGucFromDataDir;
using pgcpp::server::ServerConfig;

namespace {

// Write the given content to a temp file and return its path.
std::string WriteTempFile(const std::string& name, const std::string& content) {
    std::string path = "/tmp/" + name + "_" + std::to_string(getpid()) + ".conf";
    std::ofstream f(path);
    if (!f.is_open()) {
        return "";
    }
    f << content;
    f.close();
    return path;
}

void RemoveTempFile(const std::string& path) {
    if (!path.empty()) {
        std::remove(path.c_str());
    }
}

}  // namespace

// --- File loading ---

TEST(GucTest, LoadFileReturnsFalseWhenFileMissing) {
    GucConfig config;
    EXPECT_FALSE(config.LoadFile("/nonexistent/path/postgresql.conf"));
    EXPECT_EQ(config.size(), 0u);
}

TEST(GucTest, LoadFileEmptyFileLoadsZeroGucs) {
    std::string path = WriteTempFile("empty", "");
    ASSERT_FALSE(path.empty());
    GucConfig config;
    EXPECT_TRUE(config.LoadFile(path));
    EXPECT_EQ(config.size(), 0u);
    RemoveTempFile(path);
}

TEST(GucTest, LoadFileIgnoresCommentsAndBlankLines) {
    std::string content =
        "# This is a comment\n"
        "\n"
        "   # indented comment\n"
        "\n";
    std::string path = WriteTempFile("comments", content);
    ASSERT_FALSE(path.empty());
    GucConfig config;
    EXPECT_TRUE(config.LoadFile(path));
    EXPECT_EQ(config.size(), 0u);
    RemoveTempFile(path);
}

TEST(GucTest, LoadFileParsesSimpleKeyValue) {
    std::string content = "port = 5433\n";
    std::string path = WriteTempFile("simple", content);
    ASSERT_FALSE(path.empty());
    GucConfig config;
    ASSERT_TRUE(config.LoadFile(path));
    EXPECT_EQ(config.size(), 1u);
    EXPECT_TRUE(config.Has("port"));
    EXPECT_EQ(config.GetString("port"), "5433");
    RemoveTempFile(path);
}

TEST(GucTest, LoadFileStripsWhitespaceAroundKeyAndValue) {
    std::string content = "   port    =    5433   \n";
    std::string path = WriteTempFile("whitespace", content);
    ASSERT_FALSE(path.empty());
    GucConfig config;
    ASSERT_TRUE(config.LoadFile(path));
    EXPECT_EQ(config.GetString("port"), "5433");
    RemoveTempFile(path);
}

TEST(GucTest, LoadFileStripsSingleQuotesFromValue) {
    std::string content = "listen_addresses = '127.0.0.1'\n";
    std::string path = WriteTempFile("quoted", content);
    ASSERT_FALSE(path.empty());
    GucConfig config;
    ASSERT_TRUE(config.LoadFile(path));
    EXPECT_EQ(config.GetString("listen_addresses"), "127.0.0.1");
    RemoveTempFile(path);
}

TEST(GucTest, LoadFileParsesMultipleEntries) {
    std::string content =
        "# postgresql.conf\n"
        "port = 5433\n"
        "max_connections = 100\n"
        "listen_addresses = '127.0.0.1'\n"
        "autovacuum = on\n";
    std::string path = WriteTempFile("multi", content);
    ASSERT_FALSE(path.empty());
    GucConfig config;
    ASSERT_TRUE(config.LoadFile(path));
    EXPECT_EQ(config.size(), 4u);
    EXPECT_EQ(config.GetString("port"), "5433");
    EXPECT_EQ(config.GetString("max_connections"), "100");
    EXPECT_EQ(config.GetString("listen_addresses"), "127.0.0.1");
    EXPECT_EQ(config.GetString("autovacuum"), "on");
    RemoveTempFile(path);
}

TEST(GucTest, LoadFileLastValueWinsOnDuplicate) {
    std::string content =
        "port = 5433\n"
        "port = 5434\n";
    std::string path = WriteTempFile("dup", content);
    ASSERT_FALSE(path.empty());
    GucConfig config;
    ASSERT_TRUE(config.LoadFile(path));
    EXPECT_EQ(config.GetString("port"), "5434");
    RemoveTempFile(path);
}

// --- Value accessors ---

TEST(GucTest, GetStringReturnsDefaultWhenMissing) {
    GucConfig config;
    EXPECT_EQ(config.GetString("missing"), "");
    EXPECT_EQ(config.GetString("missing", "fallback"), "fallback");
}

TEST(GucTest, GetIntParsesNumericValue) {
    GucConfig config;
    config.LoadFromString("port = 5433\n");
    EXPECT_EQ(config.GetInt("port", 0), 5433);
}

TEST(GucTest, GetIntReturnsDefaultOnInvalidValue) {
    GucConfig config;
    config.LoadFromString("port = abc\n");
    EXPECT_EQ(config.GetInt("port", 9999), 9999);
}

TEST(GucTest, GetIntReturnsDefaultWhenMissing) {
    GucConfig config;
    EXPECT_EQ(config.GetInt("missing", 42), 42);
}

TEST(GucTest, GetBoolAcceptsOnTrueYesOneCaseInsensitive) {
    GucConfig config;
    config.LoadFromString(
        "a = on\n"
        "b = ON\n"
        "c = true\n"
        "d = TRUE\n"
        "e = yes\n"
        "f = 1\n");
    EXPECT_TRUE(config.GetBool("a", false));
    EXPECT_TRUE(config.GetBool("b", false));
    EXPECT_TRUE(config.GetBool("c", false));
    EXPECT_TRUE(config.GetBool("d", false));
    EXPECT_TRUE(config.GetBool("e", false));
    EXPECT_TRUE(config.GetBool("f", false));
}

TEST(GucTest, GetBoolAcceptsOffFalseNoZero) {
    GucConfig config;
    config.LoadFromString(
        "a = off\n"
        "b = OFF\n"
        "c = false\n"
        "d = FALSE\n"
        "e = no\n"
        "f = 0\n");
    EXPECT_FALSE(config.GetBool("a", true));
    EXPECT_FALSE(config.GetBool("b", true));
    EXPECT_FALSE(config.GetBool("c", true));
    EXPECT_FALSE(config.GetBool("d", true));
    EXPECT_FALSE(config.GetBool("e", true));
    EXPECT_FALSE(config.GetBool("f", true));
}

TEST(GucTest, GetBoolReturnsDefaultOnUnknownValue) {
    GucConfig config;
    config.LoadFromString("a = maybe\n");
    EXPECT_TRUE(config.GetBool("a", true));
    EXPECT_FALSE(config.GetBool("a", false));
}

TEST(GucTest, GetBoolReturnsDefaultWhenMissing) {
    GucConfig config;
    EXPECT_TRUE(config.GetBool("missing", true));
    EXPECT_FALSE(config.GetBool("missing", false));
}

// --- ApplyTo ServerConfig ---

TEST(GucTest, ApplyToOverridesServerConfigValues) {
    GucConfig config;
    config.LoadFromString(
        "port = 6000\n"
        "max_connections = 50\n"
        "listen_addresses = '0.0.0.0'\n");

    ServerConfig sc;
    sc.port = 5433;
    sc.max_connections = 100;
    sc.listen_addr = "127.0.0.1";

    config.ApplyTo(&sc);
    EXPECT_EQ(sc.port, 6000);
    EXPECT_EQ(sc.max_connections, 50);
    EXPECT_EQ(sc.listen_addr, "0.0.0.0");
}

TEST(GucTest, ApplyToDoesNotOverrideUnsetValues) {
    GucConfig config;
    config.LoadFromString("autovacuum = on\n");

    ServerConfig sc;
    sc.port = 5433;
    sc.max_connections = 100;
    sc.listen_addr = "127.0.0.1";

    config.ApplyTo(&sc);
    EXPECT_EQ(sc.port, 5433);
    EXPECT_EQ(sc.max_connections, 100);
    EXPECT_EQ(sc.listen_addr, "127.0.0.1");
}

// --- LoadFromString (for in-memory parsing) ---

TEST(GucTest, LoadFromStringParsesLines) {
    GucConfig config;
    config.LoadFromString("port = 5433\nmax_connections = 100\n");
    EXPECT_EQ(config.size(), 2u);
    EXPECT_EQ(config.GetString("port"), "5433");
    EXPECT_EQ(config.GetString("max_connections"), "100");
}

// --- LoadGucFromDataDir ---

TEST(GucTest, LoadGucFromDataDirLoadsPostgresqlConf) {
    std::string dir = "/tmp/pgcpp_guc_datadir_" + std::to_string(getpid());
    std::string rm = "rm -rf " + dir;
    std::system(rm.c_str());
    ASSERT_EQ(mkdir(dir.c_str(), 0700), 0);

    {
        std::ofstream f(dir + "/postgresql.conf");
        ASSERT_TRUE(f.is_open());
        f << "port = 6000\nmax_connections = 50\n";
    }

    GucConfig guc;
    ASSERT_TRUE(LoadGucFromDataDir(dir, &guc));
    EXPECT_EQ(guc.GetInt("port", 0), 6000);
    EXPECT_EQ(guc.GetInt("max_connections", 0), 50);

    std::system(rm.c_str());
}

TEST(GucTest, LoadGucFromDataDirReturnsFalseWhenMissing) {
    GucConfig guc;
    EXPECT_FALSE(LoadGucFromDataDir("/nonexistent/dir", &guc));
    EXPECT_EQ(guc.size(), 0u);
}

TEST(GucTest, LoadGucFromDataDirReturnsFalseOnEmptyDir) {
    std::string dir = "/tmp/pgcpp_guc_emptydir_" + std::to_string(getpid());
    std::string rm = "rm -rf " + dir;
    std::system(rm.c_str());
    ASSERT_EQ(mkdir(dir.c_str(), 0700), 0);

    GucConfig guc;
    EXPECT_FALSE(LoadGucFromDataDir(dir, &guc));
    EXPECT_EQ(guc.size(), 0u);

    std::system(rm.c_str());
}
