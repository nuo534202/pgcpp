// pgbench.h — TPC-B benchmark client (pgbench).
//
// Converted from PostgreSQL 15's src/bin/pgbench/.
//
// pgbench runs a simplified TPC-B workload against a pgcpp/PostgreSQL
// server: it connects via libpq, optionally initializes the pgbench_
// accounts/tellers/branches/history tables, then runs N transactions per
// client. Each transaction does:
//
//   BEGIN;
//   UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2;
//   SELECT abalance FROM pgbench_accounts WHERE aid = $2;
//   UPDATE pgbench_tellers  SET tbalance = tbalance + $1 WHERE tid = $3;
//   UPDATE pgbench_branches SET bbalance = bbalance + $1 WHERE bid = $4;
//   INSERT INTO pgbench_history (tid, bid, aid, delta, mtime)
//     VALUES ($3, $4, $2, $1, CURRENT_TIMESTAMP);
//   COMMIT;
//
// pgcpp's pgbench is single-client (no std::thread) and synchronous. The
// SQL strings are exposed as BuildInitSql / BuildTransactionSql so they
// can be unit-tested without a server.
//
// Usage:
//   pgbench [-h host] [-p port] [-U user] [-d dbname] [-i] [-t N] [-s scale]
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace pgcpp::tools {

// PgbenchOptions — inputs to the benchmark.
struct PgbenchOptions {
    std::string host = "localhost";
    int port = 5432;
    std::string user;
    std::string dbname = "pgbench";

    // Initialize mode: create the pgbench tables and populate them.
    bool initialize = false;

    // Number of transactions to run.
    int transactions = 10;

    // TPC-B scale factor (multiplier of 100k accounts per branch).
    int scale = 1;

    // Random seed (0 = derive from time()).
    unsigned int seed = 0;
};

// PgbenchResult — outcome of a benchmark run.
enum class PgbenchResult {
    kOk,
    kConnectFailed,
    kInitFailed,
    kTransactionFailed,
    kInvalidArgument,
};

// PgbenchStats — per-transaction counters accumulated by RunPgbench.
struct PgbenchStats {
    int transactions_executed = 0;
    int transactions_failed = 0;
    double elapsed_secs = 0.0;

    // Tps — transactions per second.
    double Tps() const {
        if (elapsed_secs <= 0.0)
            return 0.0;
        return static_cast<double>(transactions_executed) / elapsed_secs;
    }
};

// BuildInitSql — return the SQL statements to initialize the pgbench tables
// for the given scale factor. The first statements CREATE the four tables
// (pgbench_branches, pgbench_tellers, pgbench_accounts, pgbench_history).
// The remaining statements populate branches/tellers/accounts via INSERT.
//
// Note: the INSERT statements for accounts use a multi-row VALUES form to
// keep the statement count manageable (one INSERT per branch, each inserting
// 100k accounts). For very large scale factors, callers may want to switch
// to COPY — left as a future extension.
std::vector<std::string> BuildInitSql(int scale);

// BuildTransactionSql — return the SQL statements that make up one TPC-B
// transaction. The statements use $1..$4 placeholders for the random
// (delta, aid, tid, bid) values, which the caller supplies via ExecParams.
std::vector<std::string> BuildTransactionSql();

// FormatInt — format an integer as a string (for query parameters).
std::string FormatInt(std::int64_t v);

// RunPgbench — execute the benchmark. Returns the aggregate result and
// accumulates per-transaction stats in `stats`.
PgbenchResult RunPgbench(const PgbenchOptions& opts, std::ostream& out,
                         PgbenchStats* stats = nullptr);

}  // namespace pgcpp::tools
