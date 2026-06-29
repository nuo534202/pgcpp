// psql_variables.cpp — psql variable substitution (variables.c).
//
// Converted from PostgreSQL 15's src/bin/psql/variables.c.
//
// Maintains the map of psql string variables (\set NAME VALUE) and performs
// `:NAME` / `:'NAME'` substitution in SQL and meta-command input.
#include "tools/psql_variables.hpp"

#include <cctype>
#include <map>
#include <string>

namespace pgcpp::tools {

namespace {

// IsNameStart — true for [A-Za-z_], the legal first char of a variable name.
bool IsNameStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

// IsNameChar — true for [A-Za-z0-9_], the legal non-first char of a name.
bool IsNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

}  // namespace

PsqlVariables::PsqlVariables() {
    // Default built-in variables, matching psql's startup values.
    vars_["AUTOCOMMIT"] = "on";
    vars_["PROMPT1"] = "%n@%m %/%R%# ";
    vars_["PROMPT2"] = "%n@%m %/%R%# ";
    vars_["PROMPT3"] = ">> ";
    vars_["ON_ERROR_STOP"] = "off";
}

void PsqlVariables::Set(const std::string& name, const std::string& value) {
    vars_[name] = value;
}

void PsqlVariables::Unset(const std::string& name) {
    vars_.erase(name);
}

std::string PsqlVariables::Get(const std::string& name) const {
    auto it = vars_.find(name);
    if (it == vars_.end()) {
        return "";
    }
    return it->second;
}

bool PsqlVariables::IsSet(const std::string& name) const {
    return vars_.count(name) > 0;
}

std::string QuoteSqlString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out.push_back('\'');  // double the embedded quote
        }
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string SubstituteVariables(const std::string& text, const PsqlVariables& vars) {
    const std::map<std::string, std::string>& map = vars.All();
    std::string result;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != ':') {
            result.push_back(text[i]);
            ++i;
            continue;
        }
        // ':' — try the :'NAME' form first (it starts with a single quote).
        if (i + 1 < text.size() && text[i + 1] == '\'') {
            size_t j = i + 2;
            if (j < text.size() && IsNameStart(text[j])) {
                size_t start = j;
                while (j < text.size() && IsNameChar(text[j])) {
                    ++j;
                }
                if (j < text.size() && text[j] == '\'') {
                    // Full :'NAME' pattern matched.
                    std::string name = text.substr(start, j - start);
                    auto it = map.find(name);
                    if (it != map.end()) {
                        result += QuoteSqlString(it->second);
                    } else {
                        // Unknown variable: leave the token as-is.
                        result += text.substr(i, j - i + 1);
                    }
                    i = j + 1;
                    continue;
                }
            }
            // Not a valid :'NAME' — emit ':' and let the '\'' be handled next.
            result.push_back(':');
            ++i;
            continue;
        }
        // Try the :NAME form.
        if (i + 1 < text.size() && IsNameStart(text[i + 1])) {
            size_t start = i + 1;
            size_t j = i + 1;
            while (j < text.size() && IsNameChar(text[j])) {
                ++j;
            }
            std::string name = text.substr(start, j - start);
            auto it = map.find(name);
            if (it != map.end()) {
                result += it->second;
            } else {
                // Unknown variable: leave the token as-is.
                result += text.substr(i, j - i);
            }
            i = j;
            continue;
        }
        // ':' not followed by a valid name char — emit literally.
        result.push_back(':');
        ++i;
    }
    return result;
}

bool ParseSetCommand(const std::string& args, std::string& name, std::string& value) {
    // Skip leading whitespace.
    size_t i = 0;
    while (i < args.size() && std::isspace(static_cast<unsigned char>(args[i]))) {
        ++i;
    }
    if (i >= args.size()) {
        return false;  // empty or all whitespace
    }
    // Read the variable name (until next whitespace).
    size_t name_start = i;
    while (i < args.size() && !std::isspace(static_cast<unsigned char>(args[i]))) {
        ++i;
    }
    name = args.substr(name_start, i - name_start);
    // Skip whitespace between name and value.
    while (i < args.size() && std::isspace(static_cast<unsigned char>(args[i]))) {
        ++i;
    }
    // The value is the remainder, with trailing whitespace trimmed.
    size_t value_end = args.size();
    while (value_end > i && std::isspace(static_cast<unsigned char>(args[value_end - 1]))) {
        --value_end;
    }
    value = args.substr(i, value_end - i);
    return true;
}

}  // namespace pgcpp::tools
