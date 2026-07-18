// pgbench.cpp — TPC-B benchmark client (pgbench).
//
// Implements the SQL builders and the synchronous benchmark loop. The loop
// uses libpq's ExecParams to bind the four random values per transaction.
#include "tools/pgbench.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "libpq/libpq.hpp"

namespace pgcpp::tools {

namespace {

// Branch size constants (TPC-B defaults).
constexpr int kAccountsPerBranch = 100000;
constexpr int kTellersPerBranch = 10;

// BuildDropTableSql — return a single DROP TABLE IF EXISTS statement.
std::string BuildDropTableSql(const std::string& table) {
    return "DROP TABLE IF EXISTS " + table + ";";
}

// BuildCreateTableSql — return the CREATE TABLE statement for the named
// table with the appropriate columns and types.
std::string BuildCreateBranchesSql() {
    return "CREATE TABLE pgbench_branches ("
           "bid INTEGER PRIMARY KEY,"
           "bbalance INTEGER,"
           "filler CHAR(88)"
           ");";
}

std::string BuildCreateTellersSql() {
    return "CREATE TABLE pgbench_tellers ("
           "tid INTEGER PRIMARY KEY,"
           "bid INTEGER,"
           "tbalance INTEGER,"
           "filler CHAR(84)"
           ");";
}

std::string BuildCreateAccountsSql() {
    return "CREATE TABLE pgbench_accounts ("
           "aid BIGINT PRIMARY KEY,"
           "bid INTEGER,"
           "abalance INTEGER,"
           "filler CHAR(84)"
           ");";
}

std::string BuildCreateHistorySql() {
    return "CREATE TABLE pgbench_history ("
           "tid INTEGER,"
           "bid INTEGER,"
           "aid BIGINT,"
           "delta INTEGER,"
           "mtime TIMESTAMP"
           ");";
}

// BuildInsertBranchesSql — INSERT one row for branch `bid`.
std::string BuildInsertBranchSql(int bid) {
    std::ostringstream s;
    s << "INSERT INTO pgbench_branches (bid, bbalance, filler) VALUES (" << bid << ", 0, ' ');";
    return s.str();
}

// BuildInsertTellerSql — INSERT one row for teller `tid` in branch `bid`.
std::string BuildInsertTellerSql(int tid, int bid) {
    std::ostringstream s;
    s << "INSERT INTO pgbench_tellers (tid, bid, tbalance, filler) VALUES (" << tid << ", " << bid
      << ", 0, ' ');";
    return s.str();
}

// BuildInsertAccountSql — INSERT one row for account `aid` in branch `bid`.
std::string BuildInsertAccountSql(std::int64_t aid, int bid) {
    std::ostringstream s;
    s << "INSERT INTO pgbench_accounts (aid, bid, abalance, filler) VALUES (" << aid << ", " << bid
      << ", 0, ' ');";
    return s.str();
}

}  // namespace

std::vector<std::string> BuildInitSql(int scale) {
    std::vector<std::string> out;
    // Drop if-exists first so init is idempotent.
    out.push_back(BuildDropTableSql("pgbench_history"));
    out.push_back(BuildDropTableSql("pgbench_accounts"));
    out.push_back(BuildDropTableSql("pgbench_tellers"));
    out.push_back(BuildDropTableSql("pgbench_branches"));

    // Create the four tables.
    out.push_back(BuildCreateBranchesSql());
    out.push_back(BuildCreateTellersSql());
    out.push_back(BuildCreateAccountsSql());
    out.push_back(BuildCreateHistorySql());

    // Populate branches: `scale` branches.
    for (int bid = 1; bid <= scale; ++bid)
        out.push_back(BuildInsertBranchSql(bid));

    // Populate tellers: kTellersPerBranch per branch.
    int total_tellers = scale * kTellersPerBranch;
    for (int tid = 1; tid <= total_tellers; ++tid) {
        int bid = (tid - 1) / kTellersPerBranch + 1;
        out.push_back(BuildInsertTellerSql(tid, bid));
    }

    // Populate accounts: kAccountsPerBranch per branch. We emit one
    // INSERT per account rather than a multi-row VALUES, which keeps the
    // statement construction simple and obvious at the cost of more
    // statements. Callers that need bulk speed can switch to COPY.
    std::int64_t total_accounts = static_cast<std::int64_t>(scale) * kAccountsPerBranch;
    for (std::int64_t aid = 1; aid <= total_accounts; ++aid) {
        int bid = static_cast<int>((aid - 1) / kAccountsPerBranch + 1);
        out.push_back(BuildInsertAccountSql(aid, bid));
    }

    return out;
}

std::vector<std::string> BuildTransactionSql() {
    return {
        "BEGIN;",
        "UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2;",
        "SELECT abalance FROM pgbench_accounts WHERE aid = $2;",
        "UPDATE pgbench_tellers  SET tbalance = tbalance + $1 WHERE tid = $3;",
        "UPDATE pgbench_branches SET bbalance = bbalance + $1 WHERE bid = $4;",
        "INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) "
        "VALUES ($3, $4, $2, $1, CURRENT_TIMESTAMP);",
        "COMMIT;",
    };
}

std::string FormatInt(std::int64_t v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

PgbenchResult RunPgbench(const PgbenchOptions& opts, std::ostream& out, PgbenchStats* stats) {
    if (opts.transactions <= 0)
        return PgbenchResult::kInvalidArgument;
    if (opts.scale <= 0)
        return PgbenchResult::kInvalidArgument;

    // Connect to the server.
    pgcpp::libpq::ConnectOptions copts;
    copts.host = opts.host;
    copts.port = opts.port;
    copts.user = opts.user;
    copts.dbname = opts.dbname;
    pgcpp::libpq::PgConn conn;
    if (conn.Connect(copts) != pgcpp::libpq::ConnStatusType::kOk) {
        out << "could not connect: " << conn.ErrorMessage() << "\n";
        return PgbenchResult::kConnectFailed;
    }

    // Initialize the database if requested.
    if (opts.initialize) {
        for (const auto& sql : BuildInitSql(opts.scale)) {
            auto r = conn.Exec(sql);
            if (r.Status() == pgcpp::libpq::ExecStatusType::kFatalError) {
                out << "init failed: " << r.ErrorMessage() << "\n";
                return PgbenchResult::kInitFailed;
            }
        }
    }

    // Run the transactions.
    auto t0 = std::chrono::steady_clock::now();
    int n_ok = 0;
    int n_fail = 0;
    unsigned int seed = opts.seed != 0 ? opts.seed : static_cast<unsigned int>(std::time(nullptr));
    std::srand(seed);

    int total_accounts = opts.scale * kAccountsPerBranch;

    for (int i = 0; i < opts.transactions; ++i) {
        // Pick random (aid, bid, tid, delta).
        // delta: random 1..100000 (matches pgbench's default).
        int delta = (std::rand() % 100000) + 1;
        // aid: random account in [1, total_accounts].
        int aid_int = (std::rand() % total_accounts) + 1;
        std::int64_t aid = aid_int;
        // bid: derived from aid (account `aid` lives in branch ((aid-1)/accounts_per_branch)+1).
        int bid = ((aid_int - 1) / kAccountsPerBranch) + 1;
        // tid: random teller in this branch.
        int tid_in_branch = (std::rand() % kTellersPerBranch);
        int tid = (bid - 1) * kTellersPerBranch + tid_in_branch + 1;

        std::vector<pgcpp::libpq::Param> params = {
            {FormatInt(delta), false},
            {FormatInt(aid), false},
            {FormatInt(tid), false},
            {FormatInt(bid), false},
        };

        bool tx_failed = false;
        for (const auto& sql : BuildTransactionSql()) {
            pgcpp::libpq::PgResult r;
            if (sql.find('$') != std::string::npos) {
                r = conn.ExecParams(sql, params);
            } else {
                r = conn.Exec(sql);
            }
            if (r.Status() == pgcpp::libpq::ExecStatusType::kFatalError) {
                tx_failed = true;
                out << "transaction " << i << " failed: " << r.ErrorMessage() << "\n";
                // Try to ROLLBACK so the connection is reusable for the next tx.
                conn.Exec("ROLLBACK;");
                break;
            }
        }
        if (tx_failed)
            ++n_fail;
        else
            ++n_ok;
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    if (stats != nullptr) {
        stats->transactions_executed = n_ok;
        stats->transactions_failed = n_fail;
        stats->elapsed_secs = elapsed;
    }

    out << "transaction type: TPC-B (simulated)\n";
    out << "scaling factor: " << opts.scale << "\n";
    out << "transactions per client: " << opts.transactions << "\n";
    out << "number of transactions actually processed: " << n_ok << "\n";
    out << "failed transactions: " << n_fail << "\n";
    out << "elapsed: " << elapsed << " s\n";
    if (elapsed > 0.0)
        out << "tps = " << (static_cast<double>(n_ok) / elapsed) << "\n";

    return n_fail > 0 ? PgbenchResult::kTransactionFailed : PgbenchResult::kOk;
}

}  // namespace pgcpp::tools
