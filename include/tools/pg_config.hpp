// pg_config.h — Build configuration printer (pg_config).
//
// Converted from PostgreSQL 15's src/bin/pg_config/.
//
// pg_config prints information about how the PostgreSQL (or pgcpp) installation
// was built: bindir, libdir, includedir, version, configure flags, compiler
// flags, etc.
//
// pgcpp's pg_config prints a faithful subset:
//   --bindir         — directory containing executables
//   --libdir         — directory containing libraries
//   --includedir     — directory containing C headers
//   --pkgincludedir  — directory containing pgcpp-specific headers
//   --version        — pgcpp version string
//   --configure      — cmake configure flags (analogous to PG's ./configure)
//   --cc             — C compiler used
//   --cppflags       — C preprocessor flags
//   --cflags         — C compiler flags
//   --cxxflags       — C++ compiler flags (pgcpp extension)
//   --ldflags        — linker flags
//   --libs           — libraries linked
#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace pgcpp::tools {

// PgConfigEntry — one configuration item.
struct PgConfigEntry {
    std::string option;  // e.g. "--bindir"
    std::string value;   // e.g. "/usr/local/pgcpp/bin"
};

// GetPgConfigEntries — return all known configuration entries.
// The values are compiled-in defaults; they can be overridden by the
// PGCPP_HOME environment variable at runtime.
const std::vector<PgConfigEntry>& GetPgConfigEntries();

// FindConfigEntry — return the entry for `option` (e.g. "--bindir"), or
// nullptr if not found.
const PgConfigEntry* FindConfigEntry(const std::string& option);

// PrintConfigEntry — print a single entry to `out` (as "value\n").
// Returns true if the entry was found.
bool PrintConfigEntry(const std::string& option, std::ostream& out);

// PrintAllConfigEntries — print all entries to `out` (as "option = value\n").
void PrintAllConfigEntries(std::ostream& out);

// PrintConfigHelp — print the help text for pg_config to `out`.
void PrintConfigHelp(std::ostream& out);

}  // namespace pgcpp::tools
