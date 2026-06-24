// main.cpp — Main entry point for the MyToyDB server.
//
// Converted from PostgreSQL 15's src/backend/main/main.c.
//
// Parses command-line arguments and dispatches to the postmaster or
// bootstrap mode. The argument parsing logic is in options.cpp (linked
// into mytoydb_core) so it can be tested independently.
#include "mytoydb/server/main.h"

#include <cstdio>
#include <string>

#include "mytoydb/server/bootstrap.h"
#include "mytoydb/server/postmaster.h"

namespace mytoydb::server {

int MyToyDBMain(int argc, char* argv[]) {
    ServerOptions opts;
    if (!ParseArgs(argc, argv, &opts)) {
        PrintUsage(argv[0]);
        return 1;
    }

    if (opts.mode == ServerMode::kHelp) {
        PrintUsage(argv[0]);
        return 0;
    }

    if (opts.mode == ServerMode::kBootstrap) {
        BootstrapResult result = BootstrapCluster(opts.data_dir);
        if (result != BootstrapResult::kOk) {
            std::fprintf(stderr, "bootstrap failed: %s\n", BootstrapResultToString(result));
            return 1;
        }
        std::fprintf(stdout, "cluster initialized at %s\n", opts.data_dir.c_str());
        return 0;
    }

    // Server mode.
    ServerConfig config;
    config.data_dir = opts.data_dir;
    config.port = opts.port;
    config.listen_addr = opts.listen_addr;
    config.max_connections = opts.max_connections;

    Postmaster postmaster(std::move(config));
    return postmaster.Run();
}

}  // namespace mytoydb::server

// --- C main entry point ---
int main(int argc, char* argv[]) {
    return mytoydb::server::MyToyDBMain(argc, argv);
}
