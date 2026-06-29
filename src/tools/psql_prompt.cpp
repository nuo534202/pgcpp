// psql_prompt.cpp — Prompt formatting (prompt.c).
//
// Converted from PostgreSQL 15's src/bin/psql/prompt.c.
//
// Expands a prompt template string by substituting `%`-escapes with runtime
// state (database, user, host, port, ...) and `:VAR` / `:'VAR'` references
// with psql variable values.
#include "tools/psql_prompt.hpp"

#include <string>

#include "tools/psql_variables.hpp"

namespace pgcpp::tools {

std::string FormatPrompt(const std::string& tmpl, const PromptContext& ctx,
                         const PsqlVariables& vars) {
    // First pass: substitute :VAR / :'VAR' references from the variable map.
    std::string s = SubstituteVariables(tmpl, vars);

    // Second pass: expand %-escapes on the result.
    std::string result;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '%') {
            result.push_back(s[i]);
            ++i;
            continue;
        }
        // Trailing '%' with no following char — emit literally.
        if (i + 1 >= s.size()) {
            result.push_back('%');
            ++i;
            continue;
        }
        char c = s[i + 1];
        switch (c) {
            case '/':
                result += ctx.database;
                break;
            case '~':
                // "~" for the default/empty database, else the actual name.
                if (ctx.database.empty() || ctx.database == "postgres") {
                    result += "~";
                } else {
                    result += ctx.database;
                }
                break;
            case '#':
                result += (ctx.is_superuser ? "#" : ">");
                break;
            case 'n':
                result += ctx.user;
                break;
            case 'm': {
                // Short host name: up to (but not including) the first dot.
                size_t dot = ctx.host.find('.');
                if (dot == std::string::npos) {
                    result += ctx.host;
                } else {
                    result += ctx.host.substr(0, dot);
                }
                break;
            }
            case 'M':
                result += ctx.host;  // full host name
                break;
            case '>':
                result += std::to_string(ctx.port);
                break;
            case 'R':
                if (ctx.kind == PromptKind::kPrompt1) {
                    result += "=";
                } else if (ctx.kind == PromptKind::kPrompt2) {
                    result += (ctx.last_status != 0 ? "^" : "-");
                } else {
                    result += ">>";
                }
                break;
            case '?':
                result += std::to_string(ctx.last_status);
                break;
            case 'p':
                // PID not tracked in this port — emit nothing.
                break;
            case '%':
                result += "%";
                break;
            default:
                // Unknown escape: emit the char after '%' literally.
                result += c;
                break;
        }
        i += 2;  // consume both '%' and the escape char
    }
    return result;
}

std::string DefaultPrompt(PromptKind kind) {
    switch (kind) {
        case PromptKind::kPrompt1:
            return "%/%R%# ";
        case PromptKind::kPrompt2:
            return "%/%R%# ";
        case PromptKind::kPrompt3:
            return ">> ";
    }
    return "";
}

}  // namespace pgcpp::tools
