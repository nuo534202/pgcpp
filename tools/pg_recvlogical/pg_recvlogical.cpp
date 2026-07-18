// pg_recvlogical.cpp — main entry point for the pg_recvlogical tool.
//
// Usage:
//   pgcpp_pg_recvlogical [opts] -S <slot>
// Options:
//   -h, --host=HOST        server host
//   -p, --port=PORT        server port
//   -U, --user=USER         username
//   -d, --dbname=NAME       database name
//   -S, --slot=NAME         replication slot
//       --create-slot       create the slot
//       --drop-slot         drop the slot
//       --start             start streaming (default)
//       --stop              stop streaming
//       --slot-info         show slot info
//       --plugin=NAME       plugin name (--create-slot only)
//   -I, --startpos=LSN      start LSN
//   -f, --file=PATH         file to write stream to (default: stdout)
//   -v, --verbose           per-message output
#include <iostream>
#include <string>

#include "tools/pg_recvlogical.hpp"

namespace {

void PrintUsage() {
    std::cerr << "Usage: pg_recvlogical [opts] -S <slot>\n"
              << "  -h, --host=HOST       server host\n"
              << "  -p, --port=PORT       server port\n"
              << "  -U, --user=USER        username\n"
              << "  -d, --dbname=NAME      database name\n"
              << "  -S, --slot=NAME        replication slot\n"
              << "      --create-slot      create the slot\n"
              << "      --drop-slot        drop the slot\n"
              << "      --start            start streaming\n"
              << "      --stop             stop streaming\n"
              << "      --slot-info        show slot info\n"
              << "      --plugin=NAME      plugin name (--create-slot only)\n"
              << "  -I, --startpos=LSN    start LSN\n"
              << "  -f, --file=PATH        output file (default: stdout)\n"
              << "  -v, --verbose         verbose output\n";
}

bool MatchOpt(const std::string& arg, const std::string& short_name,
              const std::string& long_name, std::string* value) {
    std::string short_eq = short_name + "=";
    std::string long_eq = long_name + "=";
    if (arg == short_name || arg == long_name) {
        return true;
    }
    if (!short_eq.empty() && arg.compare(0, short_eq.size(), short_eq) == 0) {
        *value = arg.substr(short_eq.size());
        return true;
    }
    if (!long_eq.empty() && arg.compare(0, long_eq.size(), long_eq) == 0) {
        *value = arg.substr(long_eq.size());
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    pgcpp::tools::RecvlogicalOptions opts;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string val;
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (MatchOpt(a, "-h", "--host", &val)) {
            if (!val.empty())
                opts.host = val;
        } else if (MatchOpt(a, "-p", "--port", &val)) {
            if (!val.empty())
                opts.port = std::stoi(val);
        } else if (MatchOpt(a, "-U", "--user", &val)) {
            if (!val.empty())
                opts.user = val;
        } else if (MatchOpt(a, "-d", "--dbname", &val)) {
            if (!val.empty())
                opts.dbname = val;
        } else if (MatchOpt(a, "-S", "--slot", &val)) {
            if (!val.empty())
                opts.slot = val;
        } else if (a == "--create-slot") {
            opts.action = pgcpp::tools::RecvlogicalAction::kCreate;
        } else if (a == "--drop-slot") {
            opts.action = pgcpp::tools::RecvlogicalAction::kDrop;
        } else if (a == "--start") {
            opts.action = pgcpp::tools::RecvlogicalAction::kStart;
        } else if (a == "--stop") {
            opts.action = pgcpp::tools::RecvlogicalAction::kStop;
        } else if (a == "--slot-info") {
            opts.action = pgcpp::tools::RecvlogicalAction::kInfo;
        } else if (MatchOpt(a, "", "--plugin", &val)) {
            if (!val.empty())
                opts.plugin = val;
        } else if (MatchOpt(a, "-I", "--startpos", &val)) {
            if (!val.empty())
                opts.start_lsn = std::stoll(val);
        } else if (MatchOpt(a, "-f", "--file", &val)) {
            if (!val.empty())
                opts.output_file = val;
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (opts.slot.empty()) {
        std::cerr << "pg_recvlogical: -S/--slot is required\n";
        return 2;
    }

    pgcpp::tools::RecvlogicalStats stats;
    pgcpp::tools::RecvlogicalResult r = pgcpp::tools::RunRecvlogical(opts, stats, &std::cout);

    if (verbose && opts.action == pgcpp::tools::RecvlogicalAction::kStart) {
        std::cout << "messages: " << stats.messages_received
                  << ", txns: " << stats.transactions_received
                  << ", inserts: " << stats.inserts
                  << ", updates: " << stats.updates
                  << ", deletes: " << stats.deletes << "\n";
    }

    switch (r) {
        case pgcpp::tools::RecvlogicalResult::kOk:
            return 0;
        case pgcpp::tools::RecvlogicalResult::kConnectFailed:
            std::cerr << "pg_recvlogical: connection failed\n";
            return 1;
        case pgcpp::tools::RecvlogicalResult::kSlotMissing:
            std::cerr << "pg_recvlogical: slot not found\n";
            return 1;
        case pgcpp::tools::RecvlogicalResult::kSlotExists:
            std::cerr << "pg_recvlogical: slot already exists\n";
            return 1;
        case pgcpp::tools::RecvlogicalResult::kQueryFailed:
            std::cerr << "pg_recvlogical: query failed\n";
            return 1;
        case pgcpp::tools::RecvlogicalResult::kWriteFailed:
            std::cerr << "pg_recvlogical: write failed\n";
            return 1;
        case pgcpp::tools::RecvlogicalResult::kUnsupportedAction:
            std::cerr << "pg_recvlogical: unsupported action\n";
            return 2;
    }
    return 0;
}
