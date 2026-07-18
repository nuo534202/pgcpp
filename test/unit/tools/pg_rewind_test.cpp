// pg_rewind_test.cpp — Unit tests for the pg_rewind tool.
//
// Verifies the file-hash helper, the diff helper, the directory-walk
// enumeration, and the end-to-end RunRewind flow on temporary data
// directories. No server is required since pgcpp's pg_rewind is a
// filesystem-level sync.
#include "tools/pg_rewind.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace pt = pgcpp::tools;
using std::string;

namespace {

class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/pgcpp_rewind_test_XXXXXX";
        char* p = mkdtemp(tmpl);
        if (p != nullptr)
            path_ = p;
    }
    ~TempDir() {
        if (!path_.empty()) {
            std::string cmd = "rm -rf " + path_;
            int rc = std::system(cmd.c_str());
            (void)rc;
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const string& Path() const { return path_; }
    bool Created() const { return !path_.empty(); }

private:
    string path_;
};

string Join(const string& a, const string& b) {
    return a + "/" + b;
}

void MakeFile(const string& path, const string& contents) {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out) << "failed to open " << path;
    out << contents;
}

void MakeDir(const string& path) {
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        FAIL() << "mkdir " << path << " failed: " << std::strerror(errno);
}

void MakeDataDir(const string& path) {
    MakeDir(path);
    MakeFile(Join(path, "PG_VERSION"), "15\n");
}

}  // namespace

TEST(PgRewindTest, ComputeFileHashEmptyFile) {
    TempDir td;
    string path = Join(td.Path(), "empty");
    MakeFile(path, "");
    std::uint64_t h = pt::ComputeFileHash(path);
    // FNV offset basis (non-zero).
    EXPECT_NE(h, 0);
}

TEST(PgRewindTest, ComputeFileHashMissingFile) {
    TempDir td;
    string path = Join(td.Path(), "missing");
    std::uint64_t h = pt::ComputeFileHash(path);
    EXPECT_EQ(h, 0);
}

TEST(PgRewindTest, ComputeFileHashDeterministic) {
    TempDir td;
    string p1 = Join(td.Path(), "f1");
    string p2 = Join(td.Path(), "f2");
    MakeFile(p1, "hello world");
    MakeFile(p2, "hello world");
    EXPECT_EQ(pt::ComputeFileHash(p1), pt::ComputeFileHash(p2));
}

TEST(PgRewindTest, ComputeFileHashDiffersForDifferentContents) {
    TempDir td;
    string p1 = Join(td.Path(), "f1");
    string p2 = Join(td.Path(), "f2");
    MakeFile(p1, "hello world");
    MakeFile(p2, "hello earth");
    EXPECT_NE(pt::ComputeFileHash(p1), pt::ComputeFileHash(p2));
}

TEST(PgRewindTest, FilesDifferBySize) {
    TempDir td;
    string p1 = Join(td.Path(), "a");
    string p2 = Join(td.Path(), "b");
    MakeFile(p1, "short");
    MakeFile(p2, "much longer");
    EXPECT_TRUE(pt::FilesDiffer(p1, p2, false));
    EXPECT_TRUE(pt::FilesDiffer(p1, p2, true));  // quick
}

TEST(PgRewindTest, FilesDifferSameSizeDiffContent) {
    TempDir td;
    string p1 = Join(td.Path(), "a");
    string p2 = Join(td.Path(), "b");
    MakeFile(p1, "AAAA");
    MakeFile(p2, "BBBB");
    EXPECT_TRUE(pt::FilesDiffer(p1, p2, false));
    EXPECT_FALSE(pt::FilesDiffer(p1, p2, true));  // quick = same size
}

TEST(PgRewindTest, FilesDifferEqual) {
    TempDir td;
    string p1 = Join(td.Path(), "a");
    string p2 = Join(td.Path(), "b");
    MakeFile(p1, "same");
    MakeFile(p2, "same");
    EXPECT_FALSE(pt::FilesDiffer(p1, p2, false));
    EXPECT_FALSE(pt::FilesDiffer(p1, p2, true));
}

TEST(PgRewindTest, FilesDifferMissingFile) {
    TempDir td;
    string p1 = Join(td.Path(), "a");
    MakeFile(p1, "x");
    EXPECT_TRUE(pt::FilesDiffer(p1, Join(td.Path(), "missing"), false));
}

TEST(PgRewindTest, EnumerateDataDirFilesBasic) {
    TempDir td;
    MakeDataDir(td.Path());
    MakeFile(Join(td.Path(), "postgresql.conf"), "# config\n");
    MakeFile(Join(td.Path(), "postmaster.pid"), "1234\n");  // skipped

    auto files = pt::EnumerateDataDirFiles(td.Path());
    auto has = [&](const string& n) {
        return std::find(files.begin(), files.end(), n) != files.end();
    };
    EXPECT_TRUE(has("PG_VERSION"));
    EXPECT_TRUE(has("postgresql.conf"));
    EXPECT_FALSE(has("postmaster.pid"));
}

TEST(PgRewindTest, EnumerateDataDirFilesNested) {
    TempDir td;
    MakeDataDir(td.Path());
    MakeDir(Join(td.Path(), "base"));
    MakeDir(Join(td.Path(), "base/16384"));
    MakeFile(Join(td.Path(), "base/16384/1234"), "data");

    auto files = pt::EnumerateDataDirFiles(td.Path());
    auto has = [&](const string& n) {
        return std::find(files.begin(), files.end(), n) != files.end();
    };
    EXPECT_TRUE(has("base/16384/1234"));
}

TEST(PgRewindTest, RemoveFileRemovesExisting) {
    TempDir td;
    string p = Join(td.Path(), "to_remove");
    MakeFile(p, "x");
    EXPECT_TRUE(pt::RemoveFile(p));
    struct stat st;
    EXPECT_NE(stat(p.c_str(), &st), 0);
}

TEST(PgRewindTest, RemoveFileMissingIsOk) {
    TempDir td;
    string p = Join(td.Path(), "nonexistent");
    EXPECT_TRUE(pt::RemoveFile(p));
}

TEST(PgRewindTest, EnsureParentDirCreatesMissing) {
    TempDir td;
    string path = Join(td.Path(), "a/b/c/file");
    EXPECT_TRUE(pt::EnsureParentDir(path));
    struct stat st;
    EXPECT_EQ(stat(Join(td.Path(), "a").c_str(), &st), 0);
    EXPECT_EQ(stat(Join(td.Path(), "a/b").c_str(), &st), 0);
    EXPECT_EQ(stat(Join(td.Path(), "a/b/c").c_str(), &st), 0);
}

TEST(PgRewindTest, RunRewindInvalidSourceDir) {
    TempDir td;
    pt::RewindOptions opts;
    opts.source_dir = Join(td.Path(), "nonexistent_source");
    opts.target_dir = Join(td.Path(), "nonexistent_target");
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kInvalidSourceDir);
}

TEST(PgRewindTest, RunRewindSourceEqualsTarget) {
    TempDir td;
    MakeDataDir(td.Path());
    pt::RewindOptions opts;
    opts.source_dir = td.Path();
    opts.target_dir = td.Path();
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kSourceIsTarget);
}

TEST(PgRewindTest, RunRewindEmptyOpts) {
    pt::RewindOptions opts;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kInvalidSourceDir);
}

TEST(PgRewindTest, RunRewindCopiesNewFileFromSource) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    MakeFile(Join(src, "new_file.txt"), "new content");

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_EQ(stats.files_copied, 1);
    EXPECT_EQ(stats.files_unchanged, 1);  // PG_VERSION exists in both src and tgt

    struct stat st;
    EXPECT_EQ(stat(Join(tgt, "new_file.txt").c_str(), &st), 0);
}

TEST(PgRewindTest, RunRewindUpdatesChangedFile) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    MakeFile(Join(src, "config.txt"), "new content");
    MakeFile(Join(tgt, "config.txt"), "old content");

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_EQ(stats.files_copied, 1);
    EXPECT_EQ(stats.files_unchanged, 1);  // PG_VERSION

    // Verify the file was updated.
    std::ifstream in(Join(tgt, "config.txt"));
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "new content");
}

TEST(PgRewindTest, RunRewindRemovesFileGoneFromSource) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    MakeFile(Join(tgt, "stale_file.txt"), "old content");

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_EQ(stats.files_removed, 1);
    EXPECT_EQ(stats.files_copied, 0);
    EXPECT_EQ(stats.files_unchanged, 1);  // PG_VERSION

    struct stat st;
    EXPECT_NE(stat(Join(tgt, "stale_file.txt").c_str(), &st), 0);
}

TEST(PgRewindTest, RunRewindDryRunDoesNotModifyTarget) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    MakeFile(Join(src, "new_file.txt"), "new");
    MakeFile(Join(tgt, "stale_file.txt"), "stale");

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    opts.dry_run = true;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_EQ(stats.files_copied, 1);   // new_file.txt would be added
    EXPECT_EQ(stats.files_removed, 1);  // stale_file.txt would be removed

    // Verify neither file was actually touched.
    struct stat st;
    EXPECT_NE(stat(Join(tgt, "new_file.txt").c_str(), &st), 0);    // not added
    EXPECT_EQ(stat(Join(tgt, "stale_file.txt").c_str(), &st), 0);  // still there
}

TEST(PgRewindTest, RunRewindSkipsPostmasterPid) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    // postmaster.pid in target should NOT be removed (it's a runtime artifact).
    MakeFile(Join(tgt, "postmaster.pid"), "1234\n");

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_EQ(stats.files_removed, 0);
    struct stat st;
    EXPECT_EQ(stat(Join(tgt, "postmaster.pid").c_str(), &st), 0);  // still there
}

TEST(PgRewindTest, RunRewindQuickModeComparesSizesOnly) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    // Same size, different content.
    MakeFile(Join(src, "f.txt"), "AAAA");
    MakeFile(Join(tgt, "f.txt"), "BBBB");

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    opts.quick = true;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_EQ(stats.files_copied, 0);     // sizes match, so no update
    EXPECT_EQ(stats.files_unchanged, 2);  // PG_VERSION + f.txt

    // Without --quick, the content difference would trigger an update.
    pt::RewindStats stats2;
    pt::RewindOptions opts2 = opts;
    opts2.quick = false;
    pt::RunRewind(opts2, stats2);
    EXPECT_EQ(stats2.files_copied, 1);
}

TEST(PgRewindTest, RunRewindNestedDirsAreSynced) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    MakeDir(Join(src, "base"));
    MakeDir(Join(src, "base/16384"));
    MakeFile(Join(src, "base/16384/1234"), "data");

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_EQ(stats.files_copied, 1);

    struct stat st;
    EXPECT_EQ(stat(Join(tgt, "base/16384/1234").c_str(), &st), 0);
}

TEST(PgRewindTest, RunRewindVerboseOutputHasSummary) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    MakeFile(Join(src, "new.txt"), "new");

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    pt::RewindStats stats;
    std::ostringstream out;
    pt::RewindResult r = pt::RunRewind(opts, stats, &out);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_NE(out.str().find("rewinding:"), std::string::npos);
    EXPECT_NE(out.str().find("rewind complete"), std::string::npos);
}

TEST(PgRewindTest, RunRewindSymlinksAreCopied) {
    TempDir td;
    string src = Join(td.Path(), "src");
    string tgt = Join(td.Path(), "tgt");
    MakeDataDir(src);
    MakeDataDir(tgt);
    // Create a symlink in source pointing to a non-existent target.
    // (We don't care about target validity, just that the link is recreated.)
    ASSERT_EQ(symlink("pg_wal", Join(src, "pg_wal_symbolic").c_str()), 0);

    pt::RewindOptions opts;
    opts.source_dir = src;
    opts.target_dir = tgt;
    pt::RewindStats stats;
    pt::RewindResult r = pt::RunRewind(opts, stats);
    EXPECT_EQ(r, pt::RewindResult::kOk);
    EXPECT_EQ(stats.files_copied, 1);

    struct stat st;
    EXPECT_EQ(lstat(Join(tgt, "pg_wal_symbolic").c_str(), &st), 0);
    EXPECT_TRUE(S_ISLNK(st.st_mode));
}
