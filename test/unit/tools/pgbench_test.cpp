// pgbench_test.cpp — Unit tests for the pgbench tool.
//
// Covers:
//   - BuildInitSql: shape of the SQL for various scale factors (drop
//     statements, create statements, branch/teller/account inserts).
//   - BuildTransactionSql: the seven-statement TPC-B loop with $1..$4
//     placeholders.
//   - FormatInt: integer-to-string conversion.
//   - PgbenchStats::Tps: division-by-zero safety, basic computation.
//   - RunPgbench error paths (no server): connect failure, invalid args.
#include "tools/pgbench.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using pgcpp::tools::BuildInitSql;
using pgcpp::tools::BuildTransactionSql;
using pgcpp::tools::FormatInt;
using pgcpp::tools::PgbenchOptions;
using pgcpp::tools::PgbenchResult;
using pgcpp::tools::PgbenchStats;
using pgcpp::tools::RunPgbench;

// ===========================================================================
// BuildInitSql
// ===========================================================================

TEST(PgbenchTest, BuildInitSqlDropsExistingTables) {
    auto sql = BuildInitSql(/*scale=*/1);
    ASSERT_FALSE(sql.empty());
    // The first 4 statements are DROP TABLE IF EXISTS.
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(sql[i].find("DROP TABLE IF EXISTS"), std::string::npos)
            << "stmt " << i << ": " << sql[i];
    }
    EXPECT_NE(sql[0].find("pgbench_history"), std::string::npos);
    EXPECT_NE(sql[1].find("pgbench_accounts"), std::string::npos);
    EXPECT_NE(sql[2].find("pgbench_tellers"), std::string::npos);
    EXPECT_NE(sql[3].find("pgbench_branches"), std::string::npos);
}

TEST(PgbenchTest, BuildInitSqlCreatesFourTables) {
    auto sql = BuildInitSql(/*scale=*/1);
    // After the 4 DROPs, the next 4 are CREATE TABLE.
    for (int i = 4; i < 8; ++i) {
        EXPECT_NE(sql[i].find("CREATE TABLE"), std::string::npos) << "stmt " << i << ": " << sql[i];
    }
    EXPECT_NE(sql[4].find("pgbench_branches"), std::string::npos);
    EXPECT_NE(sql[5].find("pgbench_tellers"), std::string::npos);
    EXPECT_NE(sql[6].find("pgbench_accounts"), std::string::npos);
    EXPECT_NE(sql[7].find("pgbench_history"), std::string::npos);
}

TEST(PgbenchTest, BuildInitSqlScaleOneHasOneBranchTenTellers100kAccounts) {
    auto sql = BuildInitSql(/*scale=*/1);
    // After 4 DROPs + 4 CREATEs = 8 setup statements, the rest are INSERTs.
    // For scale=1: 1 branch + 10 tellers + 100000 accounts = 100011 INSERTs.
    int insert_count = static_cast<int>(sql.size()) - 8;
    EXPECT_EQ(insert_count, 1 + 10 + 100000);
}

TEST(PgbenchTest, BuildInitSqlScaleTwoHasDoubleEverything) {
    auto sql = BuildInitSql(/*scale=*/2);
    int insert_count = static_cast<int>(sql.size()) - 8;
    // For scale=2: 2 branches + 20 tellers + 200000 accounts.
    EXPECT_EQ(insert_count, 2 + 20 + 200000);
}

TEST(PgbenchTest, BuildInitSqlAccountAidMapsToCorrectBid) {
    auto sql = BuildInitSql(/*scale=*/2);
    // Account 1 should be in branch 1.
    bool found_aid1_bid1 = false;
    bool found_aid100001_bid2 = false;
    for (const auto& s : sql) {
        if (s.find("VALUES (1, 1, 0") != std::string::npos)
            found_aid1_bid1 = true;
        if (s.find("VALUES (100001, 2, 0") != std::string::npos)
            found_aid100001_bid2 = true;
    }
    EXPECT_TRUE(found_aid1_bid1);
    EXPECT_TRUE(found_aid100001_bid2);
}

TEST(PgbenchTest, BuildInitSqlBranchInsertUsesBid) {
    auto sql = BuildInitSql(/*scale=*/3);
    int branch_inserts = 0;
    for (const auto& s : sql) {
        if (s.find("pgbench_branches") != std::string::npos &&
            s.find("INSERT") != std::string::npos)
            ++branch_inserts;
    }
    EXPECT_EQ(branch_inserts, 3);
}

TEST(PgbenchTest, BuildInitSqlTellerInsertUsesTidBid) {
    auto sql = BuildInitSql(/*scale=*/1);
    int teller_inserts = 0;
    for (const auto& s : sql) {
        if (s.find("pgbench_tellers") != std::string::npos && s.find("INSERT") != std::string::npos)
            ++teller_inserts;
    }
    EXPECT_EQ(teller_inserts, 10);
}

// ===========================================================================
// BuildTransactionSql
// ===========================================================================

TEST(PgbenchTest, BuildTransactionSqlHasSevenStatements) {
    auto sql = BuildTransactionSql();
    EXPECT_EQ(sql.size(), 7u);
}

TEST(PgbenchTest, BuildTransactionSqlBeginsAndCommits) {
    auto sql = BuildTransactionSql();
    EXPECT_EQ(sql.front(), "BEGIN;");
    EXPECT_EQ(sql.back(), "COMMIT;");
}

TEST(PgbenchTest, BuildTransactionSqlUsesFourParams) {
    auto sql = BuildTransactionSql();
    // Each non-BEGIN/COMMIT statement should reference at least one of $1..$4.
    for (std::size_t i = 1; i + 1 < sql.size(); ++i) {
        bool has_param = false;
        for (int p = 1; p <= 4; ++p) {
            std::string placeholder = "$" + std::to_string(p);
            if (sql[i].find(placeholder) != std::string::npos) {
                has_param = true;
                break;
            }
        }
        EXPECT_TRUE(has_param) << "stmt " << i << " has no $1..$4: " << sql[i];
    }
}

TEST(PgbenchTest, BuildTransactionSqlUpdatesThreeTables) {
    auto sql = BuildTransactionSql();
    bool updates_accounts = false, updates_tellers = false, updates_branches = false;
    for (const auto& s : sql) {
        if (s.find("UPDATE pgbench_accounts") != std::string::npos)
            updates_accounts = true;
        if (s.find("UPDATE pgbench_tellers") != std::string::npos)
            updates_tellers = true;
        if (s.find("UPDATE pgbench_branches") != std::string::npos)
            updates_branches = true;
    }
    EXPECT_TRUE(updates_accounts);
    EXPECT_TRUE(updates_tellers);
    EXPECT_TRUE(updates_branches);
}

TEST(PgbenchTest, BuildTransactionSqlInsertsHistory) {
    auto sql = BuildTransactionSql();
    bool found_history_insert = false;
    for (const auto& s : sql) {
        if (s.find("INSERT INTO pgbench_history") != std::string::npos) {
            found_history_insert = true;
            // Should reference all four params.
            EXPECT_NE(s.find("$1"), std::string::npos);
            EXPECT_NE(s.find("$2"), std::string::npos);
            EXPECT_NE(s.find("$3"), std::string::npos);
            EXPECT_NE(s.find("$4"), std::string::npos);
        }
    }
    EXPECT_TRUE(found_history_insert);
}

// ===========================================================================
// FormatInt
// ===========================================================================

TEST(PgbenchTest, FormatIntBasic) {
    EXPECT_EQ(FormatInt(0), "0");
    EXPECT_EQ(FormatInt(1), "1");
    EXPECT_EQ(FormatInt(-1), "-1");
    EXPECT_EQ(FormatInt(1000), "1000");
}

TEST(PgbenchTest, FormatInt64Max) {
    EXPECT_EQ(FormatInt(INT64_C(9223372036854775807)), "9223372036854775807");
}

TEST(PgbenchTest, FormatIntInt64Min) {
    EXPECT_EQ(FormatInt(INT64_C(-9223372036854775807) - 1), "-9223372036854775808");
}

// ===========================================================================
// PgbenchStats::Tps
// ===========================================================================

TEST(PgbenchTest, TpsZeroElapsedReturnsZero) {
    PgbenchStats s;
    s.transactions_executed = 10;
    s.elapsed_secs = 0.0;
    EXPECT_EQ(s.Tps(), 0.0);
}

TEST(PgbenchTest, TpsNegativeElapsedReturnsZero) {
    PgbenchStats s;
    s.transactions_executed = 10;
    s.elapsed_secs = -1.0;
    EXPECT_EQ(s.Tps(), 0.0);
}

TEST(PgbenchTest, TpsComputation) {
    PgbenchStats s;
    s.transactions_executed = 100;
    s.elapsed_secs = 10.0;
    EXPECT_DOUBLE_EQ(s.Tps(), 10.0);
}

// ===========================================================================
// RunPgbench error paths (no server)
// ===========================================================================

TEST(PgbenchTest, RunPgbenchInvalidArgsNegativeTransactions) {
    PgbenchOptions opts;
    opts.transactions = -1;
    std::ostringstream out;
    EXPECT_EQ(RunPgbench(opts, out), PgbenchResult::kInvalidArgument);
}

TEST(PgbenchTest, RunPgbenchInvalidArgsZeroScale) {
    PgbenchOptions opts;
    opts.scale = 0;
    std::ostringstream out;
    EXPECT_EQ(RunPgbench(opts, out), PgbenchResult::kInvalidArgument);
}

TEST(PgbenchTest, RunPgbenchNoServerConnectFailed) {
    PgbenchOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 1;  // no server on port 1
    opts.transactions = 1;
    std::ostringstream out;
    EXPECT_EQ(RunPgbench(opts, out), PgbenchResult::kConnectFailed);
    EXPECT_NE(out.str().find("could not connect"), std::string::npos);
}
