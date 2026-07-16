// extension.hpp — Extension control file parsing and available extensions
// registry (P3-10).
//
// PostgreSQL extensions use a .control file (key = value format) to declare
// metadata (default version, relocatability, schema, dependencies) and one or
// more --version.sql script files that contain the actual SQL objects to
// create. pgcpp mirrors this design with an in-memory registry so that tests
// don't need filesystem access.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::extension {

// ExtensionControlFile — parsed contents of an extension's .control file.
//
// Field names mirror PostgreSQL's ExtensionControlFile struct (see
// src/backend/commands/extension.c). Unknown keys are ignored.
struct ExtensionControlFile {
    std::string name;                        // extension name (from filename)
    std::string default_version;             // default_version key
    std::string comment;                     // comment key (human-readable)
    std::string schema;                      // schema key (target schema, may be empty)
    bool relocatable = false;                // relocatable key
    bool superuser = true;                   // superuser key (default true in PG)
    bool trusted = false;                    // trusted key
    std::string module_pathname;             // module_pathname key
    std::vector<std::string> requires_list;  // requires key (comma-separated)
};

// ExtensionScript — a single --version.sql script for an extension.
struct ExtensionScript {
    std::string extname;  // extension name
    std::string version;  // version string
    std::string sql;      // SQL script content
};

// ParseControlFile — parse a .control file's text content into an
// ExtensionControlFile. The `name` field is set by the caller (from the
// filename). Returns false on parse error (malformed line).
//
// Format: one key = value per line. Lines starting with '#' are comments.
// String values may be single-quoted; quotes are stripped. Boolean values
// are 'true'/'false'. The `requires` key is comma-separated.
bool ParseControlFile(const std::string& content, ExtensionControlFile& out);

// ParseControlFileWithName — convenience wrapper that sets the name field.
bool ParseControlFileWithName(const std::string& name, const std::string& content,
                              ExtensionControlFile& out);

// ExtensionRegistry — in-memory registry of available extensions.
//
// Holds control files and version scripts. In PostgreSQL, this is done by
// scanning the extension directory ($sharedir/extension). pgcpp uses an
// in-memory registry populated at startup (or by tests) so that no
// filesystem access is needed.
class ExtensionRegistry {
public:
    ExtensionRegistry() = default;
    ~ExtensionRegistry() = default;

    ExtensionRegistry(const ExtensionRegistry&) = delete;
    ExtensionRegistry& operator=(const ExtensionRegistry&) = delete;

    // Register a control file. If a control file with the same name already
    // exists, it is replaced. Returns false if the control file is invalid.
    bool RegisterControlFile(const ExtensionControlFile& ctrl);

    // Register a version script. If a script with the same (name, version)
    // already exists, it is replaced.
    void RegisterScript(const ExtensionScript& script);

    // Look up a control file by extension name. Returns nullptr if not found.
    const ExtensionControlFile* GetControlFile(const std::string& name) const;

    // Look up a script by (extension name, version). Returns nullptr if not
    // found.
    const ExtensionScript* GetScript(const std::string& name, const std::string& version) const;

    // List all registered extension names (for pg_available_extensions).
    std::vector<std::string> ListAvailable() const;

    // Number of registered extensions.
    std::size_t Count() const { return controls_.size(); }

    // Remove all registered extensions and scripts (for test isolation).
    void Clear();

private:
    std::vector<ExtensionControlFile> controls_;
    std::vector<ExtensionScript> scripts_;
};

// GetExtensionRegistry — returns the process-wide extension registry.
// Lazily initialized on first call.
ExtensionRegistry& GetExtensionRegistry();

// RegisterBuiltinExtensions — populates the registry with built-in
// extensions. Called once at server startup. Built-in extensions provide
// simple functionality (e.g., a UUID generator, an intarray helper) that
// demonstrates the extension mechanism without requiring external files.
void RegisterBuiltinExtensions();

}  // namespace pgcpp::extension
