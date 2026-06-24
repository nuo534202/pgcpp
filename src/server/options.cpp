// options.cpp — Command-line argument parsing for the MyToyDB server.
//
// Extracted from main.cpp so that the parsing logic is available to both
// the server executable and the test suite (via mytoydb_core).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mytoydb/server/main.h"

namespace mytoydb::server {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "MyToyDB — a C++20 conversion of PostgreSQL\n\n");
    std::fprintf(stderr, "Usage:\n");
    std::fprintf(stderr, "  %s [options]                    Start the server\n", prog_name);
    std::fprintf(stderr, "  %s --bootstrap -D <dir>         Initialize a new cluster\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -D <dir>          Data directory (required)\n");
    std::fprintf(stderr, "  -p <port>         Listen port (default: 5433)\n");
    std::fprintf(stderr, "  -h <addr>         Listen address (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -N <max>          Max connections (default: 100)\n");
    std::fprintf(stderr, "  --bootstrap       Initialize a new cluster and exit\n");
    std::fprintf(stderr, "  --help            Show this help\n");
}

bool ParseArgs(int argc, char* argv[], ServerOptions* opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-?") {
            opts->mode = ServerMode::kHelp;
            return true;
        }

        if (arg == "--bootstrap") {
            opts->mode = ServerMode::kBootstrap;
            continue;
        }

        if (arg == "-D") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -D requires an argument\n");
                return false;
            }
            opts->data_dir = argv[++i];
            continue;
        }

        if (arg == "-p") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -p requires an argument\n");
                return false;
            }
            opts->port = std::atoi(argv[++i]);
            continue;
        }

        if (arg == "-h") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -h requires an argument\n");
                return false;
            }
            opts->listen_addr = argv[++i];
            continue;
        }

        if (arg == "-N") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -N requires an argument\n");
                return false;
            }
            opts->max_connections = std::atoi(argv[++i]);
            continue;
        }

        std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
        return false;
    }

    if (opts->mode != ServerMode::kHelp && opts->data_dir.empty()) {
        std::fprintf(stderr, "error: data directory (-D) is required\n");
        return false;
    }

    return true;
}

}  // namespace mytoydb::server
