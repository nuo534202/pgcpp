// main.h — Main entry point for the pgcpp server.
//
// Converted from PostgreSQL 15's src/backend/main/main.c.
//
// Dispatches between server mode (postmaster) and bootstrap mode (initdb
// equivalent) based on command-line arguments.
#pragma once

#include <string>

namespace pgcpp::server {

class GucConfig;

// ServerMode — the mode the server should run in.
enum class ServerMode {
    kServer,     // Run as a server (postmaster)
    kBootstrap,  // Bootstrap a new cluster (initdb equivalent)
    kHelp,       // Print usage and exit
};

// ServerOptions — parsed command-line options.
struct ServerOptions {
    ServerMode mode = ServerMode::kServer;
    std::string data_dir;
    int port = 5433;
    std::string listen_addr = "127.0.0.1";
    int max_connections = 100;
};

// ParseArgs — parse command-line arguments.
// Returns true if parsing succeeded, false on error.
bool ParseArgs(int argc, char* argv[], ServerOptions* opts);

// PrintUsage — print usage information to stderr.
void PrintUsage(const char* prog_name);

// LoadGucFromDataDir — load `<data_dir>/postgresql.conf` into `guc` if present.
// Returns true if the file was found and loaded, false otherwise (guc is left
// untouched on failure). Missing file is not an error.
bool LoadGucFromDataDir(const std::string& data_dir, GucConfig* guc);

// pgcppMain — the main entry point.
// Returns 0 on success, non-zero on error.
int pgcppMain(int argc, char* argv[]);

}  // namespace pgcpp::server
