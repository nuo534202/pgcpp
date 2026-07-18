// pg_basebackup_test.cpp — Unit tests for the pg_basebackup tool.
//
// Verifies the helper predicates (IsDataDir, ShouldSkipFile, IsWalFile,
// FormatBytes), the target-directory creation logic, and the end-to-end
// RunBasebackup flow on a temporary data directory. No server is required
// since pgcpp's pg_basebackup is a filesystem-level copy.
#include "tools/pg_basebackup.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace pt = pgcpp::tools;
using std::string;

namespace {

// TempDir — RAII helper that creates a unique directory and removes it on exit.
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/pgcpp_basebackup_test_XXXXXX";
        char* p = mkdtemp(tmpl);
        if (p != nullptr)
            path_ = p;
    }
    ~TempDir() {
        if (!path_.empty()) {
            std::string cmd = "rm -rf " + path_;
            int rc = std::system(cmd.c_str());
            (void)rc;  // ignore cleanup failures
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const string& Path() const { return path_; }
    bool Created() const { return !path_.empty(); }

private:
    string path_;
};

string JoinPath(const string& a, const string& b) {
    return a + "/" + b;
}

// MakeFile — write `contents` to `<dir>/<name>`, creating parent dirs.
void MakeFile(const string& path, const string& contents) {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out) << "failed to open " << path;
    out << contents;
}

// MakeDir — create a directory.
void MakeDir(const string& path) {
    ASSERT_EQ(mkdir(path.c_str(), 0700), 0) << "mkdir " << path << " failed";
}

}  // namespace

TEST(PgBasebackupTest, ShouldSkipFilePostmasterPid) {
    EXPECT_TRUE(pt::ShouldSkipFile("postmaster.pid"));
    EXPECT_TRUE(pt::ShouldSkipFile("postmaster.opts"));
}

TEST(PgBasebackupTest, ShouldSkipLogFile) {
    EXPECT_TRUE(pt::ShouldSkipFile("postgresql.log"));
    EXPECT_TRUE(pt::ShouldSkipFile("server.log"));
    EXPECT_FALSE(pt::ShouldSkipFile("postgresql.conf"));
}

TEST(PgBasebackupTest, ShouldSkipSocketFile) {
    EXPECT_TRUE(pt::ShouldSkipFile("pgsql.5432.lock"));
    EXPECT_TRUE(pt::ShouldSkipFile("pgsql.5432"));
}

TEST(PgBasebackupTest, ShouldSkipLockFile) {
    EXPECT_TRUE(pt::ShouldSkipFile("foo.lock"));
    EXPECT_FALSE(pt::ShouldSkipFile("postgresql.auto.conf"));
}

TEST(PgBasebackupTest, IsWalFileRecognizesSegment) {
    EXPECT_TRUE(pt::IsWalFile("pg_wal/000000010000000000000001"));
    EXPECT_TRUE(pt::IsWalFile("pg_wal/00000001000000000000000A"));
}

TEST(PgBasebackupTest, IsWalFileArchiveStatus) {
    EXPECT_TRUE(pt::IsWalFile("pg_wal/archive_status"));
}

TEST(PgBasebackupTest, IsWalFileRejectsOthers) {
    EXPECT_FALSE(pt::IsWalFile("postgresql.conf"));
    EXPECT_FALSE(pt::IsWalFile("base/12345/67890"));
    EXPECT_FALSE(pt::IsWalFile("pg_xlog/000000010000000000000001"));
}

TEST(PgBasebackupTest, FormatBytesBytes) {
    EXPECT_EQ(pt::FormatBytes(0), "0 B");
    EXPECT_EQ(pt::FormatBytes(1), "1 B");
    EXPECT_EQ(pt::FormatBytes(1023), "1023 B");
}

TEST(PgBasebackupTest, FormatBytesKiB) {
    EXPECT_EQ(pt::FormatBytes(1024), "1.00 KiB");
    EXPECT_EQ(pt::FormatBytes(1536), "1.50 KiB");
}

TEST(PgBasebackupTest, FormatBytesMiB) {
    EXPECT_EQ(pt::FormatBytes(1024 * 1024), "1.00 MiB");
    EXPECT_EQ(pt::FormatBytes(2 * 1024 * 1024), "2.00 MiB");
}

TEST(PgBasebackupTest, FormatBytesGiB) {
    EXPECT_EQ(pt::FormatBytes(static_cast<std::int64_t>(1024) * 1024 * 1024), "1.00 GiB");
}

TEST(PgBasebackupTest, IsDataDirWithPgVersion) {
    TempDir td;
    MakeFile(JoinPath(td.Path(), "PG_VERSION"), "15\n");
    EXPECT_TRUE(pt::IsDataDir(td.Path()));
}

TEST(PgBasebackupTest, IsDataDirWithConf) {
    TempDir td;
    MakeFile(JoinPath(td.Path(), "postgresql.conf"), "# config\n");
    EXPECT_TRUE(pt::IsDataDir(td.Path()));
}

TEST(PgBasebackupTest, IsDataDirRejectsEmptyDir) {
    TempDir td;
    EXPECT_FALSE(pt::IsDataDir(td.Path()));
}

TEST(PgBasebackupTest, EnsureTargetDirCreatesNew) {
    TempDir td;
    string target = JoinPath(td.Path(), "newdir");
    EXPECT_TRUE(pt::EnsureTargetDir(target));
    struct stat st;
    EXPECT_EQ(stat(target.c_str(), &st), 0);
    EXPECT_TRUE(S_ISDIR(st.st_mode));
}

TEST(PgBasebackupTest, EnsureTargetDirRejectsNonEmpty) {
    TempDir td;
    MakeFile(JoinPath(td.Path(), "file.txt"), "x");
    EXPECT_FALSE(pt::EnsureTargetDir(td.Path()));
}

TEST(PgBasebackupTest, EnsureTargetDirRejectsNonDir) {
    TempDir td;
    string file_path = JoinPath(td.Path(), "afile");
    MakeFile(file_path, "x");
    EXPECT_FALSE(pt::EnsureTargetDir(file_path));
}

TEST(PgBasebackupTest, RunBasebackupInvalidSourceDir) {
    TempDir td;
    string src = JoinPath(td.Path(), "nonexistent_source");
    string dst = JoinPath(td.Path(), "dst");
    pt::BasebackupOptions opts;
    opts.source_dir = src;
    opts.target_dir = dst;
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kInvalidSourceDir);
}

TEST(PgBasebackupTest, RunBasebackupSourceEqualsTarget) {
    TempDir td;
    MakeFile(JoinPath(td.Path(), "PG_VERSION"), "15\n");
    pt::BasebackupOptions opts;
    opts.source_dir = td.Path();
    opts.target_dir = td.Path();
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kSourceIsTarget);
}

TEST(PgBasebackupTest, RunBasebackupEmptySourceDir) {
    pt::BasebackupOptions opts;
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kInvalidSourceDir);
}

TEST(PgBasebackupTest, RunBasebackupCopiesFiles) {
    TempDir td;
    string src = JoinPath(td.Path(), "src");
    MakeDir(src);
    MakeFile(JoinPath(src, "PG_VERSION"), "15\n");
    MakeFile(JoinPath(src, "postgresql.conf"), "# config\n");
    MakeFile(JoinPath(src, "postmaster.pid"), "1234\n");
    string dst = JoinPath(td.Path(), "dst");

    pt::BasebackupOptions opts;
    opts.source_dir = src;
    opts.target_dir = dst;
    opts.wal_method = pt::WalMethod::kNone;
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kOk);
    EXPECT_EQ(stats.files_copied, 2);   // PG_VERSION + postgresql.conf
    EXPECT_EQ(stats.files_skipped, 1);  // postmaster.pid
    EXPECT_GT(stats.bytes_copied, 0);

    // Verify files exist on the destination.
    struct stat st;
    EXPECT_EQ(stat(JoinPath(dst, "PG_VERSION").c_str(), &st), 0);
    EXPECT_EQ(stat(JoinPath(dst, "postgresql.conf").c_str(), &st), 0);
    EXPECT_NE(stat(JoinPath(dst, "postmaster.pid").c_str(), &st), 0);
}

TEST(PgBasebackupTest, RunBasebackupCopiesWalDirWhenFetch) {
    TempDir td;
    string src = JoinPath(td.Path(), "src");
    MakeDir(src);
    MakeFile(JoinPath(src, "PG_VERSION"), "15\n");
    string wal_dir = JoinPath(src, "pg_wal");
    MakeDir(wal_dir);
    MakeFile(JoinPath(wal_dir, "000000010000000000000001"), std::string(1024, 'x'));
    string dst = JoinPath(td.Path(), "dst");

    pt::BasebackupOptions opts;
    opts.source_dir = src;
    opts.target_dir = dst;
    opts.wal_method = pt::WalMethod::kFetch;  // default
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kOk);
    // pg_wal/ dir + the segment file.
    EXPECT_EQ(stats.files_copied, 2);  // PG_VERSION + WAL segment (dir is not counted as a file)

    // Verify WAL segment was copied.
    struct stat st;
    EXPECT_EQ(stat(JoinPath(dst, "pg_wal/000000010000000000000001").c_str(), &st), 0);
}

TEST(PgBasebackupTest, RunBasebackupSkipsWalDirWhenNone) {
    TempDir td;
    string src = JoinPath(td.Path(), "src");
    MakeDir(src);
    MakeFile(JoinPath(src, "PG_VERSION"), "15\n");
    string wal_dir = JoinPath(src, "pg_wal");
    MakeDir(wal_dir);
    MakeFile(JoinPath(wal_dir, "000000010000000000000001"), std::string(1024, 'x'));
    string dst = JoinPath(td.Path(), "dst");

    pt::BasebackupOptions opts;
    opts.source_dir = src;
    opts.target_dir = dst;
    opts.wal_method = pt::WalMethod::kNone;
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kOk);
    EXPECT_EQ(stats.files_copied, 1);  // PG_VERSION only

    // Verify WAL segment was NOT copied.
    struct stat st;
    EXPECT_NE(stat(JoinPath(dst, "pg_wal/000000010000000000000001").c_str(), &st), 0);
}

TEST(PgBasebackupTest, RunBasebackupDryRunDoesNotCopy) {
    TempDir td;
    string src = JoinPath(td.Path(), "src");
    MakeDir(src);
    MakeFile(JoinPath(src, "PG_VERSION"), "15\n");
    string dst = JoinPath(td.Path(), "dst");

    pt::BasebackupOptions opts;
    opts.source_dir = src;
    opts.target_dir = dst;
    opts.dry_run = true;
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kOk);
    EXPECT_EQ(stats.files_copied, 1);
    EXPECT_EQ(stats.bytes_copied, 3);  // "15\n"

    // Verify destination was not created.
    struct stat st;
    EXPECT_NE(stat(dst.c_str(), &st), 0);
}

TEST(PgBasebackupTest, RunBasebackupNestedDirsAreCopied) {
    TempDir td;
    string src = JoinPath(td.Path(), "src");
    MakeDir(src);
    MakeFile(JoinPath(src, "PG_VERSION"), "15\n");
    MakeDir(JoinPath(src, "base"));
    MakeDir(JoinPath(src, "base/16384"));
    MakeFile(JoinPath(src, "base/16384/1234"), "data");
    string dst = JoinPath(td.Path(), "dst");

    pt::BasebackupOptions opts;
    opts.source_dir = src;
    opts.target_dir = dst;
    opts.wal_method = pt::WalMethod::kNone;
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kOk);
    // PG_VERSION + base/16384/1234 (dirs don't count)
    EXPECT_EQ(stats.files_copied, 2);

    struct stat st;
    EXPECT_EQ(stat(JoinPath(dst, "base/16384/1234").c_str(), &st), 0);
}

TEST(PgBasebackupTest, RunBasebackupGzipProducesCompressedFiles) {
    TempDir td;
    string src = JoinPath(td.Path(), "src");
    MakeDir(src);
    MakeFile(JoinPath(src, "PG_VERSION"), "15\n");
    MakeFile(JoinPath(src, "postgresql.conf"), std::string(4096, 'A'));  // highly compressible
    string dst = JoinPath(td.Path(), "dst");

    pt::BasebackupOptions opts;
    opts.source_dir = src;
    opts.target_dir = dst;
    opts.wal_method = pt::WalMethod::kNone;
    opts.gzip = true;
    opts.compression_level = 9;
    pt::BasebackupStats stats;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats);
    EXPECT_EQ(r, pt::BasebackupResult::kOk);
    EXPECT_EQ(stats.files_copied, 2);

    // Verify .gz files exist on the destination.
    struct stat st;
    EXPECT_EQ(stat(JoinPath(dst, "PG_VERSION.gz").c_str(), &st), 0);
    EXPECT_EQ(stat(JoinPath(dst, "postgresql.conf.gz").c_str(), &st), 0);
    // Verify the uncompressed file is NOT present.
    EXPECT_NE(stat(JoinPath(dst, "postgresql.conf").c_str(), &st), 0);
}

TEST(PgBasebackupTest, RunBasebackupVerboseOutputHasAtLeastOneLine) {
    TempDir td;
    string src = JoinPath(td.Path(), "src");
    MakeDir(src);
    MakeFile(JoinPath(src, "PG_VERSION"), "15\n");
    string dst = JoinPath(td.Path(), "dst");

    pt::BasebackupOptions opts;
    opts.source_dir = src;
    opts.target_dir = dst;
    opts.wal_method = pt::WalMethod::kNone;
    pt::BasebackupStats stats;
    std::ostringstream out;
    pt::BasebackupResult r = pt::RunBasebackup(opts, stats, &out);
    EXPECT_EQ(r, pt::BasebackupResult::kOk);
    EXPECT_NE(out.str().find("starting base backup"), std::string::npos);
    EXPECT_NE(out.str().find("base backup complete"), std::string::npos);
}
