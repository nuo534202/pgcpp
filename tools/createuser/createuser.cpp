// createuser.cpp — pgcpp CREATE ROLE utility (createuser equivalent).
//
// Converted from PostgreSQL 15's src/bin/createuser/.
//
// Connects to a running server and issues a CREATE ROLE statement.
//
// Usage:
//   createuser [-h host] [-p port] [-d connect_db] <name>
//              [--superuser] [--createdb] [--createrole] [--replication]
//              [--no-login] [-P password]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pgcpp/tools/sql_admin.hpp"

using pgcpp::tools::AdminResult;
using pgcpp::tools::CreateRole;
using pgcpp::tools::CreateuserOptions;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp create-user utility (createuser equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options] <username>\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>          Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>          Server port (default: 5433)\n");
    std::fprintf(stderr, "  -d <database>      Database to connect to (default: pgcpp)\n");
    std::fprintf(stderr, "  --superuser        SUPERUSER attribute\n");
    std::fprintf(stderr, "  --createdb         CREATEDB attribute\n");
    std::fprintf(stderr, "  --createrole       CREATEROLE attribute\n");
    std::fprintf(stderr, "  --replication      REPLICATION attribute\n");
    std::fprintf(stderr, "  --no-login         NOLOGIN attribute\n");
    std::fprintf(stderr, "  -P <password>      Encrypted password\n");
    std::fprintf(stderr, "  --help             Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    CreateuserOptions opts;
    std::string host = "127.0.0.1";
    int port = 5433;
    std::string connect_db = "pgcpp";
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
            connect_db = argv[++i];
        } else if (arg == "--superuser") {
            opts.superuser = true;
        } else if (arg == "--createdb") {
            opts.createdb = true;
        } else if (arg == "--createrole") {
            opts.createrole = true;
        } else if (arg == "--replication") {
            opts.replication = true;
        } else if (arg == "--no-login") {
            opts.login = false;
        } else if (arg == "-P" || arg == "--password") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -P requires an argument\n");
                return 1;
            }
            opts.password = argv[++i];
        } else if (arg[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        } else if (opts.name.empty()) {
            opts.name = arg;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
            return 1;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }
    if (opts.name.empty()) {
        std::fprintf(stderr, "error: username is required\n");
        PrintUsage(argv[0]);
        return 1;
    }

    AdminResult r = CreateRole(host, port, connect_db, opts);
    if (r != AdminResult::kOk) {
        std::fprintf(stderr, "createuser: failed (code %d)\n", static_cast<int>(r));
        return 1;
    }
    std::fprintf(stdout, "CREATE ROLE\n");
    return 0;
}
