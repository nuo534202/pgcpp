// pg_upgrade_test.cpp — Unit tests for the pg_upgrade tool.
//
// Verifies the version-file reader, compatibility checker, relation-file
// predicate, the new-cluster data-presence check, and the end-to-end
// RunUpgrade flow on temporary data directories.
#include "tools/pg_upgrade.hpp"

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
        char tmpl[] = "/tmp/pgcpp_upgrade_test_XXXXXX";
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

void MakeDataDir(const string& path, int version = 15) {
    MakeDir(path);
    MakeFile(Join(path, "PG_VERSION"), std::to_string(version) + "\n");
}

}  // namespace

TEST(PgUpgradeTest, ReadVersionFileBasic) {
    TempDir td;
    MakeDataDir(td.Path(), 15);
    EXPECT_EQ(pt::ReadVersionFile(td.Path()), 15);
}

TEST(PgUpgradeTest, ReadVersionFileMissing) {
    TempDir td;
    EXPECT_EQ(pt::ReadVersionFile(td.Path()), 0);
}

TEST(PgUpgradeTest, ReadVersionFileMalformed) {
    TempDir td;
    MakeFile(Join(td.Path(), "PG_VERSION"), "abc");
    EXPECT_EQ(pt::ReadVersionFile(td.Path()), 0);
}

TEST(PgUpgradeTest, CheckCompatibilitySameVersion) {
    EXPECT_TRUE(pt::CheckCompatibility(15, 15));
    EXPECT_TRUE(pt::CheckCompatibility(14, 14));
}

TEST(PgUpgradeTest, CheckCompatibilityDifferentVersion) {
    EXPECT_FALSE(pt::CheckCompatibility(14, 15));
    EXPECT_FALSE(pt::CheckCompatibility(15, 14));
}

TEST(PgUpgradeTest, CheckCompatibilityZero) {
    EXPECT_FALSE(pt::CheckCompatibility(0, 15));
    EXPECT_FALSE(pt::CheckCompatibility(15, 0));
}

TEST(PgUpgradeTest, IsClusterRunningNoPid) {
    TempDir td;
    MakeDataDir(td.Path());
    EXPECT_FALSE(pt::IsClusterRunning(td.Path()));
}

TEST(PgUpgradeTest, IsClusterRunningWithPid) {
    TempDir td;
    MakeDataDir(td.Path());
    MakeFile(Join(td.Path(), "postmaster.pid"), "1234\n");
    EXPECT_TRUE(pt::IsClusterRunning(td.Path()));
}

TEST(PgUpgradeTest, IsRelationFileNumeric) {
    EXPECT_TRUE(pt::IsRelationFile("16384"));
    EXPECT_TRUE(pt::IsRelationFile("16385"));
    EXPECT_TRUE(pt::IsRelationFile("1"));
}

TEST(PgUpgradeTest, IsRelationFileWithSuffix) {
    EXPECT_TRUE(pt::IsRelationFile("16384_fsm"));
    EXPECT_TRUE(pt::IsRelationFile("16384_vm"));
    EXPECT_TRUE(pt::IsRelationFile("16384_init"));
}

TEST(PgUpgradeTest, IsRelationFileRejectsNonNumeric) {
    EXPECT_FALSE(pt::IsRelationFile("postgresql.conf"));
    EXPECT_FALSE(pt::IsRelationFile(""));
    EXPECT_FALSE(pt::IsRelationFile("abc"));
    EXPECT_FALSE(pt::IsRelationFile("PG_VERSION"));
}

TEST(PgUpgradeTest, IsRelationFileRejectsBadSuffix) {
    EXPECT_FALSE(pt::IsRelationFile("16384_xyz"));
    EXPECT_FALSE(pt::IsRelationFile("16384_fsm2"));
}

TEST(PgUpgradeTest, NewClusterHasUserDataEmpty) {
    TempDir td;
    MakeDataDir(td.Path());
    EXPECT_FALSE(pt::NewClusterHasUserData(td.Path()));
}

TEST(PgUpgradeTest, NewClusterHasUserDataWithRelation) {
    TempDir td;
    MakeDataDir(td.Path());
    MakeDir(Join(td.Path(), "base"));
    MakeDir(Join(td.Path(), "base/16384"));
    MakeFile(Join(td.Path(), "base/16384/1234"), "data");
    EXPECT_TRUE(pt::NewClusterHasUserData(td.Path()));
}

TEST(PgUpgradeTest, NewClusterHasUserDataWithEmptyRelation) {
    TempDir td;
    MakeDataDir(td.Path());
    MakeDir(Join(td.Path(), "base"));
    MakeDir(Join(td.Path(), "base/16384"));
    MakeFile(Join(td.Path(), "base/16384/1234"), "");  // empty file
    EXPECT_FALSE(pt::NewClusterHasUserData(td.Path()));
}

TEST(PgUpgradeTest, NewClusterHasUserDataWithNonRelation) {
    TempDir td;
    MakeDataDir(td.Path());
    MakeDir(Join(td.Path(), "base"));
    MakeDir(Join(td.Path(), "base/16384"));
    MakeFile(Join(td.Path(), "base/16384/readme.txt"), "data");
    EXPECT_FALSE(pt::NewClusterHasUserData(td.Path()));
}

TEST(PgUpgradeTest, RunUpgradeInvalidOldDir) {
    TempDir td;
    pt::UpgradeOptions opts;
    opts.old_dir = Join(td.Path(), "nonexistent");
    opts.new_dir = td.Path();
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kInvalidOldDir);
}

TEST(PgUpgradeTest, RunUpgradeSameDirectory) {
    TempDir td;
    MakeDataDir(td.Path());
    pt::UpgradeOptions opts;
    opts.old_dir = td.Path();
    opts.new_dir = td.Path();
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kSameDirectory);
}

TEST(PgUpgradeTest, RunUpgradeVersionMismatch) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 14);
    MakeDataDir(new_dir, 15);
    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    opts.check_only = true;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kVersionMismatch);
}

TEST(PgUpgradeTest, RunUpgradeOldClusterRunning) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    MakeFile(Join(old_dir, "postmaster.pid"), "1234\n");
    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    opts.check_only = true;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kOldClusterRunning);
}

TEST(PgUpgradeTest, RunUpgradeCheckOnlyOk) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    opts.check_only = true;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kOk);
    EXPECT_EQ(stats.files_copied, 0);
}

TEST(PgUpgradeTest, RunUpgradeNewClusterNotEmpty) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    MakeDir(Join(new_dir, "base"));
    MakeDir(Join(new_dir, "base/16384"));
    MakeFile(Join(new_dir, "base/16384/1234"), "data");
    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kNewClusterNotEmpty);
}

TEST(PgUpgradeTest, RunUpgradeCopiesRelationFiles) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    MakeDir(Join(old_dir, "base"));
    MakeDir(Join(old_dir, "base/16384"));
    MakeFile(Join(old_dir, "base/16384/1234"), "data1");
    MakeFile(Join(old_dir, "base/16384/1235"), "data2");
    // Make new base/ dir too.
    MakeDir(Join(new_dir, "base"));
    MakeDir(Join(new_dir, "base/16384"));

    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kOk);
    EXPECT_EQ(stats.files_copied, 2);
    EXPECT_EQ(stats.bytes_migrated, 10);

    struct stat st;
    EXPECT_EQ(stat(Join(new_dir, "base/16384/1234").c_str(), &st), 0);
    EXPECT_EQ(stat(Join(new_dir, "base/16384/1235").c_str(), &st), 0);
}

TEST(PgUpgradeTest, RunUpgradeLinksRelationFiles) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    MakeDir(Join(old_dir, "base"));
    MakeDir(Join(old_dir, "base/16384"));
    MakeFile(Join(old_dir, "base/16384/1234"), "data");
    MakeDir(Join(new_dir, "base"));
    MakeDir(Join(new_dir, "base/16384"));

    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    opts.mode = pt::UpgradeMode::kLink;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kOk);
    EXPECT_EQ(stats.files_linked, 1);  // only 1234 is a relation file
    EXPECT_EQ(stats.files_copied, 0);

    struct stat st;
    EXPECT_EQ(stat(Join(new_dir, "base/16384/1234").c_str(), &st), 0);
    // Verify hard-link: same inode as source.
    struct stat src_st;
    EXPECT_EQ(stat(Join(old_dir, "base/16384/1234").c_str(), &src_st), 0);
    EXPECT_EQ(st.st_ino, src_st.st_ino);
}

TEST(PgUpgradeTest, RunUpgradeClonesRelationFiles) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    MakeDir(Join(old_dir, "base"));
    MakeDir(Join(old_dir, "base/16384"));
    // Make a 4096-byte file (large enough to be worth cloning).
    MakeFile(Join(old_dir, "base/16384/1234"), std::string(4096, 'A'));
    MakeDir(Join(new_dir, "base"));
    MakeDir(Join(new_dir, "base/16384"));

    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    opts.mode = pt::UpgradeMode::kClone;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kOk);
    // Cloning on /tmp may fall back to copy if filesystem doesn't support reflinks.
    EXPECT_GE(stats.files_copied + stats.files_cloned, 1);
    struct stat st;
    EXPECT_EQ(stat(Join(new_dir, "base/16384/1234").c_str(), &st), 0);
    EXPECT_EQ(st.st_size, 4096);
}

TEST(PgUpgradeTest, RunUpgradeSkipsNonRelationFiles) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    MakeDir(Join(old_dir, "base"));
    MakeDir(Join(old_dir, "base/16384"));
    MakeFile(Join(old_dir, "base/16384/1234"), "data");
    MakeFile(Join(old_dir, "base/16384/readme.txt"), "ignore");
    MakeDir(Join(new_dir, "base"));
    MakeDir(Join(new_dir, "base/16384"));

    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kOk);
    EXPECT_EQ(stats.files_copied, 1);
    EXPECT_EQ(stats.files_skipped, 1);

    struct stat st;
    EXPECT_EQ(stat(Join(new_dir, "base/16384/1234").c_str(), &st), 0);
    EXPECT_NE(stat(Join(new_dir, "base/16384/readme.txt").c_str(), &st), 0);
}

TEST(PgUpgradeTest, RunUpgradeNoBaseDirOk) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    // No base/ dir in old cluster.

    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    pt::UpgradeStats stats;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats);
    EXPECT_EQ(r, pt::UpgradeResult::kOk);
    EXPECT_EQ(stats.files_copied, 0);
}

TEST(PgUpgradeTest, RunUpgradeVerboseOutputHasSummary) {
    TempDir td;
    string old_dir = Join(td.Path(), "old");
    string new_dir = Join(td.Path(), "new");
    MakeDataDir(old_dir, 15);
    MakeDataDir(new_dir, 15);
    MakeDir(Join(old_dir, "base"));
    MakeDir(Join(old_dir, "base/16384"));
    MakeFile(Join(old_dir, "base/16384/1234"), "data");
    MakeDir(Join(new_dir, "base"));
    MakeDir(Join(new_dir, "base/16384"));

    pt::UpgradeOptions opts;
    opts.old_dir = old_dir;
    opts.new_dir = new_dir;
    pt::UpgradeStats stats;
    std::ostringstream out;
    pt::UpgradeResult r = pt::RunUpgrade(opts, stats, &out);
    EXPECT_EQ(r, pt::UpgradeResult::kOk);
    EXPECT_NE(out.str().find("old version:"), std::string::npos);
    EXPECT_NE(out.str().find("upgrade complete"), std::string::npos);
}
