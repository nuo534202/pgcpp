// extension.cpp — Extension control file parsing and registry (P3-10).
//
// Implements the .control file parser and the in-memory extension registry.
// Built-in extensions are registered at server startup to demonstrate the
// extension mechanism without requiring filesystem access.
#include "extension/extension.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace pgcpp::extension {

namespace {

// Trim leading/trailing whitespace from a string.
std::string Trim(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

// Strip surrounding single quotes from a value (if present).
std::string StripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Parse a boolean value: 'true'/'false' (case-insensitive).
bool ParseBool(const std::string& s, bool& out) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower == "true") {
        out = true;
        return true;
    }
    if (lower == "false") {
        out = false;
        return true;
    }
    return false;
}

// Split a comma-separated string into a vector of trimmed strings.
std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> result;
    std::string current;
    for (char c : s) {
        if (c == ',') {
            std::string trimmed = Trim(current);
            if (!trimmed.empty()) {
                result.push_back(trimmed);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    std::string trimmed = Trim(current);
    if (!trimmed.empty()) {
        result.push_back(trimmed);
    }
    return result;
}

}  // namespace

bool ParseControlFile(const std::string& content, ExtensionControlFile& out) {
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        // Strip trailing \r (in case of CRLF line endings).
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = Trim(line);
        // Skip empty lines and comments.
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        // Find the '=' separator.
        std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            return false;  // malformed line
        }
        std::string key = Trim(trimmed.substr(0, eq));
        std::string value = Trim(trimmed.substr(eq + 1));

        if (key == "default_version") {
            out.default_version = StripQuotes(value);
        } else if (key == "comment") {
            out.comment = StripQuotes(value);
        } else if (key == "schema") {
            out.schema = StripQuotes(value);
        } else if (key == "relocatable") {
            bool b = false;
            if (!ParseBool(value, b)) {
                return false;
            }
            out.relocatable = b;
        } else if (key == "superuser") {
            bool b = false;
            if (!ParseBool(value, b)) {
                return false;
            }
            out.superuser = b;
        } else if (key == "trusted") {
            bool b = false;
            if (!ParseBool(value, b)) {
                return false;
            }
            out.trusted = b;
        } else if (key == "module_pathname") {
            out.module_pathname = StripQuotes(value);
        } else if (key == "requires") {
            out.requires_list = SplitComma(StripQuotes(value));
        }
        // Unknown keys are silently ignored (PostgreSQL behavior).
    }
    return true;
}

bool ParseControlFileWithName(const std::string& name, const std::string& content,
                              ExtensionControlFile& out) {
    out = ExtensionControlFile{};
    out.name = name;
    return ParseControlFile(content, out);
}

// --- ExtensionRegistry ---

bool ExtensionRegistry::RegisterControlFile(const ExtensionControlFile& ctrl) {
    if (ctrl.name.empty()) {
        return false;
    }
    // Replace if already exists.
    for (auto& existing : controls_) {
        if (existing.name == ctrl.name) {
            existing = ctrl;
            return true;
        }
    }
    controls_.push_back(ctrl);
    return true;
}

void ExtensionRegistry::RegisterScript(const ExtensionScript& script) {
    for (auto& existing : scripts_) {
        if (existing.extname == script.extname && existing.version == script.version) {
            existing = script;
            return;
        }
    }
    scripts_.push_back(script);
}

const ExtensionControlFile* ExtensionRegistry::GetControlFile(const std::string& name) const {
    for (const auto& ctrl : controls_) {
        if (ctrl.name == name) {
            return &ctrl;
        }
    }
    return nullptr;
}

const ExtensionScript* ExtensionRegistry::GetScript(const std::string& name,
                                                    const std::string& version) const {
    for (const auto& script : scripts_) {
        if (script.extname == name && script.version == version) {
            return &script;
        }
    }
    return nullptr;
}

std::vector<std::string> ExtensionRegistry::ListAvailable() const {
    std::vector<std::string> result;
    result.reserve(controls_.size());
    for (const auto& ctrl : controls_) {
        result.push_back(ctrl.name);
    }
    return result;
}

void ExtensionRegistry::Clear() {
    controls_.clear();
    scripts_.clear();
}

// --- Process-wide registry ---

ExtensionRegistry& GetExtensionRegistry() {
    static ExtensionRegistry registry;
    return registry;
}

// --- Built-in extensions ---

void RegisterBuiltinExtensions() {
    auto& reg = GetExtensionRegistry();

    // pgcpp_uuid: a simple UUID-like helper (returns a random-looking int8
    // derived from a counter). Demonstrates a function-providing extension.
    {
        ExtensionControlFile ctrl{};
        ctrl.name = "pgcpp_uuid";
        ctrl.default_version = "1.0";
        ctrl.comment = "pgcpp UUID generator extension (demonstration)";
        ctrl.relocatable = true;
        ctrl.superuser = false;
        reg.RegisterControlFile(ctrl);

        ExtensionScript script{};
        script.extname = "pgcpp_uuid";
        script.version = "1.0";
        script.sql = "";
        reg.RegisterScript(script);
    }

    // pgcpp_intarray: a stub demonstrating an extension that declares a
    // schema. The actual SQL is empty (objects would be created here).
    {
        ExtensionControlFile ctrl{};
        ctrl.name = "pgcpp_intarray";
        ctrl.default_version = "1.0";
        ctrl.comment = "pgcpp integer array utilities (demonstration)";
        ctrl.schema = "public";
        ctrl.relocatable = false;
        ctrl.superuser = false;
        reg.RegisterControlFile(ctrl);

        ExtensionScript script{};
        script.extname = "pgcpp_intarray";
        script.version = "1.0";
        script.sql = "";
        reg.RegisterScript(script);
    }

    // pgcpp_test: a minimal extension with a dependency, used in tests.
    {
        ExtensionControlFile ctrl{};
        ctrl.name = "pgcpp_test";
        ctrl.default_version = "1.0";
        ctrl.comment = "pgcpp test extension";
        ctrl.relocatable = true;
        ctrl.superuser = false;
        ctrl.requires_list = {"pgcpp_uuid"};
        reg.RegisterControlFile(ctrl);

        ExtensionScript script{};
        script.extname = "pgcpp_test";
        script.version = "1.0";
        script.sql = "";
        reg.RegisterScript(script);
    }
}

}  // namespace pgcpp::extension
