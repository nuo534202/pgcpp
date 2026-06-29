// pg_restore.cpp — Database restore utility (pg_restore).
//
// Reads a plain SQL dump (produced by pg_dump), splits it into statements
// (respecting single-quoted strings, dollar-quoted blocks, and COPY ... FROM
// stdin data sections), and replays each statement via PsqlClient.
#include "pgcpp/tools/pg_restore.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pgcpp::tools {

namespace {

// Trim leading and trailing whitespace from `s`.
std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(start, end - start);
}

// Lowercase a string (ASCII only).
std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Try to match a dollar-quote opening delimiter at position `pos` in `s`.
// A delimiter is `$` + optional tag (alnum/underscore) + `$`.
// Returns the full delimiter string (e.g. "$$" or "$tag$") on success and
// sets `*end_pos` to one past the delimiter; returns "" on no match.
std::string MatchDollarQuote(const std::string& s, size_t pos, size_t* end_pos) {
    if (pos >= s.size() || s[pos] != '$')
        return "";
    size_t i = pos + 1;
    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
        ++i;
    if (i >= s.size() || s[i] != '$')
        return "";
    *end_pos = i + 1;
    return s.substr(pos, i + 1 - pos);
}

}  // namespace

bool IsCopyStatement(const std::string& stmt) {
    std::string trimmed = Trim(stmt);
    if (trimmed.empty())
        return false;
    // Consider text up to the first ';' (the COPY header line).
    size_t semi = trimmed.find(';');
    std::string before = (semi == std::string::npos) ? trimmed : trimmed.substr(0, semi);
    std::string lower = ToLower(before);
    // Liberal check: starts with "COPY" and contains "FROM stdin".
    if (lower.rfind("copy", 0) != 0)
        return false;
    return lower.find("from stdin") != std::string::npos;
}

bool IsDataStatement(const std::string& stmt) {
    std::string lower = ToLower(Trim(stmt));
    if (lower.rfind("copy ", 0) == 0)
        return true;
    if (lower.rfind("insert into", 0) == 0)
        return true;
    return false;
}

std::vector<std::string> SplitDumpIntoStatements(const std::string& dump) {
    std::vector<std::string> result;
    std::string current;
    bool in_single = false;
    bool in_dollar = false;
    std::string dollar_tag;

    size_t i = 0;
    size_t n = dump.size();
    while (i < n) {
        char c = dump[i];

        if (in_single) {
            if (c == '\\' && i + 1 < n) {
                // Backslash escape: include the next char literally.
                current.push_back(c);
                current.push_back(dump[i + 1]);
                i += 2;
                continue;
            }
            if (c == '\'') {
                current.push_back(c);
                if (i + 1 < n && dump[i + 1] == '\'') {
                    // Doubled single quote — stay in string.
                    current.push_back(dump[i + 1]);
                    i += 2;
                    continue;
                }
                in_single = false;
                ++i;
                continue;
            }
            current.push_back(c);
            ++i;
            continue;
        }

        if (in_dollar) {
            if (c == '$' && i + dollar_tag.size() <= n &&
                dump.compare(i, dollar_tag.size(), dollar_tag) == 0) {
                // Closing delimiter found.
                current.append(dollar_tag);
                i += dollar_tag.size();
                in_dollar = false;
                dollar_tag.clear();
                continue;
            }
            current.push_back(c);
            ++i;
            continue;
        }

        // Not inside any quote.
        if (c == '\'') {
            in_single = true;
            current.push_back(c);
            ++i;
            continue;
        }

        if (c == '$') {
            size_t end_pos = 0;
            std::string tag = MatchDollarQuote(dump, i, &end_pos);
            if (!tag.empty()) {
                in_dollar = true;
                dollar_tag = tag;
                current.append(tag);
                i = end_pos;
                continue;
            }
            current.push_back(c);
            ++i;
            continue;
        }

        if (c == ';') {
            current.push_back(c);
            ++i;
            std::string stmt = Trim(current);
            current.clear();
            if (stmt.empty())
                continue;

            // If this is a COPY ... FROM stdin block, consume the data rows
            // up to and including the "\." terminator line.
            if (IsCopyStatement(stmt)) {
                std::string data_block;
                while (i < n) {
                    size_t line_start = i;
                    while (i < n && dump[i] != '\n')
                        ++i;
                    std::string line;
                    if (i < n) {
                        line = dump.substr(line_start, i - line_start + 1);  // include '\n'
                        ++i;
                    } else {
                        line = dump.substr(line_start);
                    }
                    data_block.append(line);
                    // Check whether this line (minus its line ending) is "\.".
                    std::string content = line;
                    if (!content.empty() && content.back() == '\n')
                        content.pop_back();
                    if (!content.empty() && content.back() == '\r')
                        content.pop_back();
                    if (content == "\\.")
                        break;
                }
                stmt.append(data_block);
            }

            result.push_back(stmt);
            continue;
        }

        current.push_back(c);
        ++i;
    }

    return result;
}

RestoreResult RestoreDump(PsqlClient& client, std::istream& in, const RestoreOptions& opts) {
    // Read the entire stream into a string.
    std::string dump((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::vector<std::string> statements = SplitDumpIntoStatements(dump);
    bool any_failed = false;

    for (const auto& stmt : statements) {
        if (opts.schema_only && IsDataStatement(stmt))
            continue;
        if (opts.data_only && !IsDataStatement(stmt))
            continue;

        QueryResult r = client.ExecuteQuery(stmt);
        if (!r.success) {
            if (opts.exit_on_error)
                return RestoreResult::kStatementFailed;
            any_failed = true;
        }
    }

    return any_failed ? RestoreResult::kStatementFailed : RestoreResult::kOk;
}

RestoreResult RestoreDumpFromFile(PsqlClient& client, const std::string& path,
                                  const RestoreOptions& opts) {
    std::ifstream in(path);
    if (!in.is_open())
        return RestoreResult::kReadFailed;
    return RestoreDump(client, in, opts);
}

}  // namespace pgcpp::tools
