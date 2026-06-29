// pg_dump.cpp — pgcpp database dump utility (pg_dump equivalent).
//
// Converted from PostgreSQL 15's src/bin/pg_dump/.
//
// Connects to a running server and writes a SQL dump to stdout (or a file)
// that can be replayed by psql or pg_restore.
//
// Usage:
//   pg_dump [-h host] [-p port] [-d database]
//           [--schema-only] [--data-only] [--clean] [--inserts]
//           [-t table_pattern] [-f output_file]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "tools/pg_dump.hpp"

using pgcpp::tools::DumpDatabase;
using pgcpp::tools::DumpOptions;
using pgcpp::tools::DumpResult;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp database dump (pg_dump equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [options]\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -h <host>          Server host (default: 127.0.0.1)\n");
    std::fprintf(stderr, "  -p <port>          Server port (default: 5433)\n");
    std::fprintf(stderr, "  -d <database>      Database to dump (default: pgcpp)\n");
    std::fprintf(stderr, "  -t <pattern>       Only dump tables matching pattern\n");
    std::fprintf(stderr, "  -f <file>          Output file (default: stdout)\n");
    std::fprintf(stderr, "  --schema-only      Dump schema only (no data)\n");
    std::fprintf(stderr, "  --data-only        Dump data only (no schema)\n");
    std::fprintf(stderr, "  --clean            Include DROP statements before CREATE\n");
    std::fprintf(stderr, "  --inserts          Use INSERT instead of COPY for data\n");
    std::fprintf(stderr, "  --help             Show this help\n");
}

const char* ResultToString(DumpResult r) {
    switch (r) {
        case DumpResult::kOk: return "ok";
        case DumpResult::kConnectFailed: return "connection failed";
        case DumpResult::kCatalogQueryFailed: return "catalog query failed";
        case DumpResult::kNoTablesFound: return "no tables found";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char* argv[]) {
    DumpOptions opts;
    opts.database = "pgcpp";
    std::string host = "127.0.0.1";
    int port = 5433;
    std::string output_file;
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
        } else if (arg == "-t") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -t requires an argument\n");
                return 1;
            }
            opts.table_pattern = argv[++i];
        } else if (arg == "-f" || arg == "--file") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -f requires an argument\n");
                return 1;
            }
            output_file = argv[++i];
        } else if (arg == "--schema-only") {
            opts.schema_only = true;
        } else if (arg == "--data-only") {
            opts.data_only = true;
        } else if (arg == "--clean") {
            opts.clean = true;
        } else if (arg == "--inserts") {
            opts.inserts = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    std::unique_ptr<std::ostream> file_out;
    std::ostream* out = &std::cout;
    if (!output_file.empty()) {
        file_out = std::make_unique<std::ofstream>(output_file);
        if (!static_cast<std::ofstream*>(file_out.get())->is_open()) {
            std::fprintf(stderr, "error: cannot open '%s'\n", output_file.c_str());
            return 1;
        }
        out = file_out.get();
    }

    DumpResult r = DumpDatabase(host, port, opts, *out);
    if (r != DumpResult::kOk) {
        std::fprintf(stderr, "pg_dump failed: %s\n", ResultToString(r));
        return 1;
    }
    return 0;
}
