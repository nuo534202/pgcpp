// pg_config.cpp — Build configuration printer (pg_config).
//
// Returns compiled-in defaults describing how the pgcpp installation was built:
// bindir, libdir, includedir, version, configure flags, compiler flags, etc.
#include "tools/pg_config.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace pgcpp::tools {

const std::vector<PgConfigEntry>& GetPgConfigEntries() {
    static const std::vector<PgConfigEntry> entries = {
        {"--bindir", "/usr/local/pgcpp/bin"},
        {"--libdir", "/usr/local/pgcpp/lib"},
        {"--includedir", "/usr/local/pgcpp/include"},
        {"--pkgincludedir", "/usr/local/pgcpp/include/pgcpp"},
        {"--includedir-server", "/usr/local/pgcpp/include/pgcpp/server"},
        {"--version", "pgcpp (PostgreSQL) 15.0-pgcpp"},
        {"--configure", "cmake -DCMAKE_BUILD_TYPE=Release"},
        {"--cc", "gcc"},
        {"--cppflags", "-I/usr/local/pgcpp/include"},
        {"--cflags", "-Wall -O2"},
        {"--cxxflags", "-Wall -Wextra -std=c++20 -O2"},
        {"--ldflags", "-L/usr/local/pgcpp/lib"},
        {"--libs", "-lpgcpp"},
    };
    return entries;
}

const PgConfigEntry* FindConfigEntry(const std::string& option) {
    const auto& entries = GetPgConfigEntries();
    for (const auto& entry : entries) {
        if (entry.option == option)
            return &entry;
    }
    return nullptr;
}

bool PrintConfigEntry(const std::string& option, std::ostream& out) {
    const PgConfigEntry* entry = FindConfigEntry(option);
    if (entry == nullptr)
        return false;
    out << entry->value << "\n";
    return true;
}

void PrintAllConfigEntries(std::ostream& out) {
    for (const auto& entry : GetPgConfigEntries())
        out << entry.option << " = " << entry.value << "\n";
}

void PrintConfigHelp(std::ostream& out) {
    out << "Usage: pg_config <option>\n\n"
        << "Options:\n"
        << "  --bindir             show directory containing executables\n"
        << "  --libdir             show directory containing libraries\n"
        << "  --includedir         show directory containing C headers\n"
        << "  --pkgincludedir      show directory containing pgcpp-specific headers\n"
        << "  --includedir-server  show directory containing server headers\n"
        << "  --version            show pgcpp version string\n"
        << "  --configure          show cmake configure flags\n"
        << "  --cc                 show C compiler used\n"
        << "  --cppflags           show C preprocessor flags\n"
        << "  --cflags             show C compiler flags\n"
        << "  --cxxflags           show C++ compiler flags\n"
        << "  --ldflags            show linker flags\n"
        << "  --libs               show libraries linked\n";
}

}  // namespace pgcpp::tools
