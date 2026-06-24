// initdb.cpp — MyToyDB cluster initialization tool (initdb equivalent).
//
// Converted from PostgreSQL 15's src/bin/initdb/.
//
// Creates a new MyToyDB data directory and initializes the system catalog.
// This is a thin wrapper around BootstrapCluster from the server module.
//
// Usage:
//   initdb -D <data_dir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mytoydb/server/bootstrap.h"

using mytoydb::server::BootstrapCluster;
using mytoydb::server::BootstrapResult;
using mytoydb::server::BootstrapResultToString;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "MyToyDB cluster initialization (initdb equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s -D <data_dir>\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -D <dir>    Data directory (required)\n");
    std::fprintf(stderr, "  --help      Show this help\n");
}

struct InitdbOptions {
    std::string data_dir;
    bool show_help = false;
};

bool ParseArgs(int argc, char* argv[], InitdbOptions* opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            opts->show_help = true;
            return true;
        }
        if (arg == "-D") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -D requires an argument\n");
                return false;
            }
            opts->data_dir = argv[++i];
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            return false;
        }
    }
    if (!opts->show_help && opts->data_dir.empty()) {
        std::fprintf(stderr, "error: data directory (-D) is required\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    InitdbOptions opts;
    if (!ParseArgs(argc, argv, &opts)) {
        PrintUsage(argv[0]);
        return 1;
    }

    if (opts.show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    BootstrapResult result = BootstrapCluster(opts.data_dir);
    if (result != BootstrapResult::kOk) {
        std::fprintf(stderr, "initdb failed: %s\n", BootstrapResultToString(result));
        return 1;
    }

    std::fprintf(stdout,
                 "cluster successfully initialized at %s\n"
                 "You can now start the server with:\n"
                 "  mytoydb_server -D %s\n",
                 opts.data_dir.c_str(), opts.data_dir.c_str());
    return 0;
}
