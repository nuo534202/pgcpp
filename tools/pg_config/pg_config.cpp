// pg_config.cpp — pgcpp build-configuration printer (pg_config equivalent).
//
// Converted from PostgreSQL 15's src/bin/pg_config/.
//
// Prints information about how the pgcpp installation was built.
//
// Usage:
//   pg_config [<option>]
//   pg_config --help
//
// If no option is given, prints all configuration entries.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "pgcpp/tools/pg_config.hpp"

using pgcpp::tools::PrintAllConfigEntries;
using pgcpp::tools::PrintConfigEntry;
using pgcpp::tools::PrintConfigHelp;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp build-configuration printer (pg_config equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s [<option>]\n", prog_name);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  --bindir, --libdir, --includedir, --pkgincludedir\n");
    std::fprintf(stderr, "  --version, --configure, --cc, --cppflags, --cflags,\n");
    std::fprintf(stderr, "  --cxxflags, --ldflags, --libs\n");
    std::fprintf(stderr, "  --help     Show this help\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 1) {
        PrintAllConfigEntries(std::cout);
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--help-config") {
            PrintConfigHelp(std::cout);
            return 0;
        }
        if (!PrintConfigEntry(arg, std::cout)) {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
    }
    return 0;
}
