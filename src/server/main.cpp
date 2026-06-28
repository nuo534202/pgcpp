// main.cpp — Main entry point for the pgcpp server.
//
// Converted from PostgreSQL 15's src/backend/main/main.c.
//
// Parses command-line arguments and dispatches to the postmaster or
// bootstrap mode. The argument parsing logic is in options.cpp (linked
// into pgcpp_core) so it can be tested independently.
#include "pgcpp/server/main.hpp"

#include <cstdio>
#include <string>

#include "pgcpp/server/bootstrap.hpp"
#include "pgcpp/server/guc.hpp"
#include "pgcpp/server/postmaster.hpp"

namespace pgcpp::server {

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
    // Convert command-line options to the server config first.
    ServerConfig config;
    config.data_dir = opts.data_dir;
    config.port = opts.port;
    config.listen_addr = opts.listen_addr;
    config.max_connections = opts.max_connections;

    // Apply postgresql.conf overrides from the data directory if present.
    // A missing postgresql.conf is not an error (the file is optional).
    GucConfig guc;
    if (LoadGucFromDataDir(opts.data_dir, &guc)) {
        guc.ApplyTo(&config);
    }

    Postmaster postmaster(std::move(config));
    return postmaster.Run();
}

}  // namespace pgcpp::server

// --- C main entry point ---
int main(int argc, char* argv[]) {
    return pgcpp::server::MyToyDBMain(argc, argv);
}
