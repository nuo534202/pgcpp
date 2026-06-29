// psql_completion.cpp — Tab-completion for psql.
#include "tools/psql_completion.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace pgcpp::tools {

namespace {

const std::vector<std::string>& SqlKeywordsImpl() {
    static const std::vector<std::string> kKeywords = {
        "ABORT",        "ALTER",        "ANALYZE",
        "AND",          "AS",           "ASC",
        "BEGIN",        "BETWEEN",      "BY",
        "CASE",         "CAST",         "CHECK",
        "CLUSTER",      "COLUMN",       "COMMIT",
        "COPY",         "CREATE",       "CROSS",
        "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP",
        "CURRENT_USER", "DATABASE",     "DEFAULT",
        "DELETE",       "DESC",         "DISTINCT",
        "DROP",         "ELSE",         "END",
        "EXCEPT",       "EXISTS",       "EXPLAIN",
        "FALSE",        "FETCH",        "FROM",
        "FULL",         "GRANT",        "GROUP",
        "HAVING",       "IN",           "INDEX",
        "INNER",        "INSERT",       "INTERSECT",
        "INTO",         "IS",           "JOIN",
        "LEFT",         "LIKE",         "LIMIT",
        "LOCK",         "NATURAL",      "NOT",
        "NULL",         "OFFSET",       "ON",
        "ONLY",         "OR",           "ORDER",
        "OUTER",        "PRIMARY",      "REINDEX",
        "REVOKE",       "RIGHT",        "ROLLBACK",
        "ROLE",         "SELECT",       "SET",
        "TABLE",        "TEMP",         "TEMPORARY",
        "THEN",         "TO",           "TRUNCATE",
        "TRUE",         "UNION",        "UNIQUE",
        "UPDATE",       "USER",         "USING",
        "VACUUM",       "VALUES",       "VIEW",
        "WHEN",         "WHERE",        "WITH",
        "WORK",
    };
    return kKeywords;
}

const std::vector<std::string>& MetaCommandsImpl() {
    static const std::vector<std::string> kCommands = {
        "\\?",         "\\connect",    "\\copy",     "\\crosstabview",
        "\\d",         "\\da",         "\\db",       "\\dc",
        "\\dd",        "\\dD",         "\\des",      "\\df",
        "\\dg",        "\\di",         "\\dl",       "\\dL",
        "\\dn",        "\\do",         "\\dO",       "\\dp",
        "\\drds",      "\\ds",         "\\dS",       "\\dt",
        "\\dT",        "\\du",         "\\dv",       "\\dE",
        "\\dx",        "\\dy",         "\\echo",     "\\edit",
        "\\ef",        "\\errverbose", "\\f",        "\\g",
        "\\gexec",     "\\gset",       "\\h",        "\\help",
        "\\H",         "\\i",          "\\ir",       "\\l",
        "\\lo_export", "\\lo_import",  "\\lo_list",  "\\lo_unlink",
        "\\o",         "\\p",          "\\password", "\\pset",
        "\\q",         "\\quit",       "\\r",        "\\s",
        "\\set",       "\\setenv",     "\\show",     "\\t",
        "\\T",         "\\timing",     "\\unset",    "\\x",
        "\\watch",     "\\z",
    };
    return kCommands;
}

const std::vector<std::string>& PsetOptionsImpl() {
    static const std::vector<std::string> kOpts = {
        "border",
        "columns",
        "expanded",
        "fieldsep",
        "fieldsep_zero",
        "footer",
        "format",
        "linestyle",
        "null",
        "numericlocale",
        "pager",
        "pager_min_lines",
        "recordsep",
        "recordsep_zero",
        "tableattr",
        "title",
        "tuples_only",
        "unicode_border_linestyle",
        "unicode_column_linestyle",
        "unicode_header_linestyle",
    };
    return kOpts;
}

std::string ToUpper(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(s[i])) !=
            std::toupper(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

const std::vector<std::string>& SqlKeywords() {
    return SqlKeywordsImpl();
}
const std::vector<std::string>& MetaCommands() {
    return MetaCommandsImpl();
}

std::string CommonPrefix(const std::vector<std::string>& words) {
    if (words.empty())
        return "";
    std::string prefix = words[0];
    for (size_t i = 1; i < words.size(); ++i) {
        size_t j = 0;
        while (j < prefix.size() && j < words[i].size() && prefix[j] == words[i][j]) {
            ++j;
        }
        prefix = prefix.substr(0, j);
        if (prefix.empty())
            break;
    }
    return prefix;
}

CompletionResult CompleteMetaCommand(const std::string& word) {
    CompletionResult r;
    std::string w = ToLower(word);
    for (const auto& cmd : MetaCommandsImpl()) {
        std::string lower = ToLower(cmd);
        if (lower.size() >= w.size() && lower.substr(0, w.size()) == w) {
            r.candidates.push_back(cmd);
        }
    }
    std::sort(r.candidates.begin(), r.candidates.end());
    r.common_prefix = CommonPrefix(r.candidates);
    return r;
}

CompletionResult CompletePsetOption(const std::string& word) {
    CompletionResult r;
    for (const auto& opt : PsetOptionsImpl()) {
        if (StartsWithIgnoreCase(opt, word)) {
            r.candidates.push_back(opt);
        }
    }
    std::sort(r.candidates.begin(), r.candidates.end());
    r.common_prefix = CommonPrefix(r.candidates);
    return r;
}

CompletionResult CompleteSqlCommand(const CompletionContext& ctx) {
    CompletionResult r;
    // Find the word at the cursor.
    int cursor = ctx.cursor;
    if (cursor < 0)
        cursor = 0;
    if (cursor > static_cast<int>(ctx.line.size()))
        cursor = ctx.line.size();
    int end = cursor;
    int start = cursor;
    while (start > 0) {
        char c = ctx.line[start - 1];
        if (std::isspace(static_cast<unsigned char>(c)))
            break;
        --start;
    }
    std::string word = ctx.line.substr(start, end - start);

    // Detect "\pset <option>" context.
    // Look back for "\pset " before the cursor.
    std::string before = ctx.line.substr(0, end);
    if (before.find("\\pset ") != std::string::npos &&
        before.find("\\pset ") == before.find_last_of('\\')) {
        // Actually check that the last backslash command is \pset.
        size_t pset_pos = before.rfind("\\pset ");
        if (pset_pos != std::string::npos) {
            // Make sure there's no other backslash command after \pset.
            std::string after_pset = before.substr(pset_pos);
            if (after_pset.find('\\', 1) == std::string::npos) {
                return CompletePsetOption(word);
            }
        }
    }

    // If word is empty, suggest nothing (PG would suggest all keywords).
    if (word.empty())
        return r;

    std::string upper_word = ToUpper(word);
    // Match SQL keywords.
    for (const auto& kw : SqlKeywordsImpl()) {
        if (kw.size() >= upper_word.size() && kw.substr(0, upper_word.size()) == upper_word) {
            r.candidates.push_back(kw);
        }
    }
    // Match known table names (case-insensitive prefix match).
    for (const auto& t : ctx.table_names) {
        if (StartsWithIgnoreCase(t, word)) {
            r.candidates.push_back(t);
        }
    }
    // Match known column names.
    for (const auto& c : ctx.column_names) {
        if (StartsWithIgnoreCase(c, word)) {
            r.candidates.push_back(c);
        }
    }
    std::sort(r.candidates.begin(), r.candidates.end());
    r.candidates.erase(std::unique(r.candidates.begin(), r.candidates.end()), r.candidates.end());
    r.common_prefix = CommonPrefix(r.candidates);
    return r;
}

CompletionResult CompleteLine(const CompletionContext& ctx) {
    // If the line starts with a backslash, complete a meta-command.
    if (!ctx.line.empty() && ctx.line[0] == '\\') {
        // For "\pset ", complete the option.
        std::string line = ctx.line.substr(0, ctx.cursor);
        if (line.size() >= 6 && line.substr(0, 6) == "\\pset ") {
            std::string word = line.substr(6);
            return CompletePsetOption(word);
        }
        // Otherwise complete the command name (without args).
        // Find the end of the command word (first space or end of line).
        size_t sp = line.find(' ');
        std::string cmd = sp == std::string::npos ? line : line.substr(0, sp);
        if (sp == std::string::npos) {
            return CompleteMetaCommand(cmd);
        }
        // Otherwise we're past the command name — no completion.
        return CompletionResult{};
    }
    return CompleteSqlCommand(ctx);
}

}  // namespace pgcpp::tools
