// pg_restore.cpp — pgcpp database restore utility (pg_restore equivalent).
//
// Converted from PostgreSQL 15's src/bin/pg_restore/.
//
// Reads a SQL dump file (produced by pg_dump) and replays it against a
// running server.
//
// Usage:
//   pg_restore [-h host] [-p port] [-d database]
//              [-f input_file] [--schema-only] [--data-only]
//              [--exit-on-error]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "tools/pg_restore.hpp"

using pgcpp::tools::PsqlClient;
using pgcpp::tools::RestoreDump;
using pgcpp::tools::RestoreDumpFromFile;
using pgcpp::tools::RestoreOptions;
using pgcpp::tools::RestoreResult;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp database restore (pg_restore equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options] [file]\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>          Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>          Server port (default: 5433)\n");
    std::fprintf(stderr, "  -d <database>      Database to restore into\n");
    std::fprintf(stderr, "  -f <file>          Input dump file (default: stdin)\n");
    std::fprintf(stderr, "  --schema-only      Only replay DDL (skip data)\n");
    std::fprintf(stderr, "  --data-only        Only replay data (skip DDL)\n");
    std::fprintf(stderr, "  --exit-on-error    Stop on first error\n");
    std::fprintf(stderr, "  --help             Show this help\n");
}

const char* ResultToString(RestoreResult r) {
    switch (r) {
        case RestoreResult::kOk: return "ok";
        case RestoreResult::kConnectFailed: return "connection failed";
        case RestoreResult::kReadFailed: return "read failed";
        case RestoreResult::kStatementFailed: return "statement failed";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char* argv[]) {
    RestoreOptions opts;
    opts.database = "pgcpp";
    std::string host = "127.0.0.1";
    int port = 5433;
    std::string input_file;
    bool show_help = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            show_help = true;
        } else if (arg == "-h") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -h requires an argument\n");
                return 1;
            }
            host = argv[++i];
        } else if (arg == "-p") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -p requires an argument\n");
                return 1;
            }
            port = std::atoi(argv[++i]);
        } else if (arg == "-d" || arg == "--dbname") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -d requires an argument\n");
                return 1;
            }
            opts.database = argv[++i];
        } else if (arg == "-f" || arg == "--file") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -f requires an argument\n");
                return 1;
            }
            input_file = argv[++i];
        } else if (arg == "--schema-only") {
            opts.schema_only = true;
        } else if (arg == "--data-only") {
            opts.data_only = true;
        } else if (arg == "--exit-on-error") {
            opts.exit_on_error = true;
        } else if (arg[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        } else if (input_file.empty()) {
            input_file = arg;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
            return 1;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    PsqlClient client(host, port);
    if (!client.Connect()) {
        std::fprintf(stderr, "pg_restore: could not connect to %s:%d\n",
                     host.c_str(), port);
        return 1;
    }

    RestoreResult r;
    if (input_file.empty()) {
        r = RestoreDump(client, std::cin, opts);
    } else {
        r = RestoreDumpFromFile(client, input_file, opts);
    }

    client.Disconnect();
    if (r != RestoreResult::kOk) {
        std::fprintf(stderr, "pg_restore: %s\n", ResultToString(r));
        return 1;
    }
    return 0;
}
