// pg_isready.cpp — pgcpp server readiness probe (pg_isready equivalent).
//
// Converted from PostgreSQL 15's src/bin/pg_isready/.
//
// Connects to a pgcpp server and reports whether it is accepting connections.
//
// Usage:
//   pg_isready [-h host] [-p port] [-d database] [-t timeout_secs]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pgcpp/tools/pg_isready.hpp"

using pgcpp::tools::CheckServerReady;
using pgcpp::tools::IsReadyOptions;
using pgcpp::tools::ReadyState;
using pgcpp::tools::ReadyStateToExitCode;
using pgcpp::tools::ReadyStateToString;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp server readiness probe (pg_isready equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options]\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>          Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>          Server port (default: 5433)\n");
    std::fprintf(stderr, "  -d <database>      Database name\n");
    std::fprintf(stderr, "  -U <user>          User name\n");
    std::fprintf(stderr, "  -t <secs>          Connect timeout (default: 3)\n");
    std::fprintf(stderr, "  --help             Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    IsReadyOptions opts;
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
            opts.host = argv[++i];
        } else if (arg == "-p") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -p requires an argument\n");
                return 1;
            }
            opts.port = std::atoi(argv[++i]);
        } else if (arg == "-d" || arg == "--dbname") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -d requires an argument\n");
                return 1;
            }
            opts.database = argv[++i];
        } else if (arg == "-U" || arg == "--username") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -U requires an argument\n");
                return 1;
            }
            opts.user = argv[++i];
        } else if (arg == "-t" || arg == "--timeout") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -t requires an argument\n");
                return 1;
            }
            opts.timeout_secs = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 3;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    ReadyState state = CheckServerReady(opts);
    std::fprintf(stdout, "%s:%d - %s\n",
                 opts.host.c_str(), opts.port, ReadyStateToString(state));
    return ReadyStateToExitCode(state);
}
