// psql_print.cpp — Query-result output formatting (print.c).
#include "pgcpp/tools/psql_print.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace pgcpp::tools {

bool ParseFormatName(const std::string& name, OutputFormat& out) {
    if (name == "aligned") {
        out = OutputFormat::kAligned;
        return true;
    }
    if (name == "unaligned") {
        out = OutputFormat::kUnaligned;
        return true;
    }
    if (name == "csv") {
        out = OutputFormat::kCsv;
        return true;
    }
    if (name == "json") {
        out = OutputFormat::kJson;
        return true;
    }
    if (name == "html") {
        out = OutputFormat::kHtml;
        return true;
    }
    if (name == "latex" || name == "latex-longtable") {
        out = OutputFormat::kLatex;
        return true;
    }
    return false;
}

std::string FormatName(OutputFormat format) {
    switch (format) {
        case OutputFormat::kAligned:
            return "aligned";
        case OutputFormat::kUnaligned:
            return "unaligned";
        case OutputFormat::kCsv:
            return "csv";
        case OutputFormat::kJson:
            return "json";
        case OutputFormat::kHtml:
            return "html";
        case OutputFormat::kLatex:
            return "latex";
    }
    return "unknown";
}

std::string EscapeCsvField(const std::string& s) {
    bool need_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            need_quote = true;
            break;
        }
    }
    if (!need_quote)
        return s;
    std::string out;
    out.push_back('"');
    for (char c : s) {
        if (c == '"')
            out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string EscapeHtml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

std::string EscapeLatex(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\':
                out += "\\textbackslash{}";
                break;
            case '%':
                out += "\\%";
                break;
            case '$':
                out += "\\$";
                break;
            case '&':
                out += "\\&";
                break;
            case '#':
                out += "\\#";
                break;
            case '_':
                out += "\\_";
                break;
            case '{':
                out += "\\{";
                break;
            case '}':
                out += "\\}";
                break;
            case '~':
                out += "\\textasciitilde{}";
                break;
            case '^':
                out += "\\textasciicircum{}";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

std::string TruncateCell(const std::string& s, int max_width) {
    if (max_width <= 0)
        return s;
    // Treat as byte count (sufficient for ASCII; PG counts display columns).
    if (static_cast<int>(s.size()) <= max_width)
        return s;
    if (max_width <= 3)
        return s.substr(0, max_width);
    return s.substr(0, max_width - 3) + "...";
}

namespace {

// Compute display width of each column (max of header and cells).
std::vector<int> ComputeColumnWidths(const QueryResult& result, int max_width) {
    int n = static_cast<int>(result.column_names.size());
    std::vector<int> widths(n, 0);
    for (int i = 0; i < n; ++i) {
        widths[i] = static_cast<int>(result.column_names[i].size());
    }
    for (const auto& row : result.rows) {
        for (int i = 0; i < n && i < static_cast<int>(row.size()); ++i) {
            int w = static_cast<int>(TruncateCell(row[i], max_width).size());
            if (w > widths[i])
                widths[i] = w;
        }
    }
    return widths;
}

}  // namespace

void PrintAligned(const QueryResult& result, const PrintOptions& opts, std::ostream& out) {
    int n = static_cast<int>(result.column_names.size());
    if (n == 0) {
        // Non-SELECT result: just print the command tag.
        if (!result.command_tag.empty())
            out << result.command_tag << "\n";
        return;
    }
    auto widths = ComputeColumnWidths(result, opts.max_width);

    if (opts.expanded) {
        // Expanded mode: one record per block.
        int idx = 0;
        for (const auto& row : result.rows) {
            out << "-[ Record " << (++idx) << " ]\n";
            for (int i = 0; i < n; ++i) {
                std::string cell = i < static_cast<int>(row.size()) ? row[i] : "";
                out << std::left << std::setw(widths[i]) << result.column_names[i] << " | "
                    << TruncateCell(cell, opts.max_width) << "\n";
            }
        }
        if (opts.show_footer) {
            out << "(" << result.rows.size() << (result.rows.size() == 1 ? " row" : " rows")
                << ")\n";
        }
        return;
    }

    // Header row.
    if (opts.show_headers) {
        out << " ";
        for (int i = 0; i < n; ++i) {
            if (i > 0)
                out << " | ";
            out << std::left << std::setw(widths[i]) << result.column_names[i];
        }
        out << "\n";
        // Separator line.
        out << "-";
        for (int i = 0; i < n; ++i) {
            if (i > 0)
                out << "-+-";
            out << std::string(widths[i], '-');
        }
        out << "-\n";
    }

    // Data rows.
    for (const auto& row : result.rows) {
        out << " ";
        for (int i = 0; i < n; ++i) {
            if (i > 0)
                out << " | ";
            std::string cell = i < static_cast<int>(row.size()) ? row[i] : "";
            out << std::left << std::setw(widths[i]) << TruncateCell(cell, opts.max_width);
        }
        out << "\n";
    }

    if (opts.show_footer) {
        out << "(" << result.rows.size() << (result.rows.size() == 1 ? " row" : " rows") << ")\n";
    }
}

void PrintUnaligned(const QueryResult& result, const PrintOptions& opts, std::ostream& out) {
    int n = static_cast<int>(result.column_names.size());
    if (opts.show_headers && n > 0) {
        for (int i = 0; i < n; ++i) {
            if (i > 0)
                out << opts.field_sep;
            out << result.column_names[i];
        }
        out << opts.record_sep;
    }
    for (const auto& row : result.rows) {
        for (int i = 0; i < static_cast<int>(row.size()); ++i) {
            if (i > 0)
                out << opts.field_sep;
            out << row[i];
        }
        out << opts.record_sep;
    }
}

void PrintCsv(const QueryResult& result, std::ostream& out) {
    int n = static_cast<int>(result.column_names.size());
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            if (i > 0)
                out << ",";
            out << EscapeCsvField(result.column_names[i]);
        }
        out << "\n";
    }
    for (const auto& row : result.rows) {
        for (int i = 0; i < static_cast<int>(row.size()); ++i) {
            if (i > 0)
                out << ",";
            out << EscapeCsvField(row[i]);
        }
        out << "\n";
    }
}

void PrintJson(const QueryResult& result, std::ostream& out) {
    out << "[";
    bool first_row = true;
    for (const auto& row : result.rows) {
        if (!first_row)
            out << ",";
        first_row = false;
        out << "{";
        for (int i = 0; i < static_cast<int>(result.column_names.size()); ++i) {
            if (i > 0)
                out << ",";
            std::string cell = i < static_cast<int>(row.size()) ? row[i] : "";
            out << "\"" << EscapeJson(result.column_names[i]) << "\":\"" << EscapeJson(cell)
                << "\"";
        }
        out << "}";
    }
    out << "]";
    out << "\n";
}

void PrintHtml(const QueryResult& result, std::ostream& out) {
    out << "<table border=\"1\">\n";
    if (!result.column_names.empty()) {
        out << "  <tr>";
        for (const auto& col : result.column_names) {
            out << "<th>" << EscapeHtml(col) << "</th>";
        }
        out << "</tr>\n";
    }
    for (const auto& row : result.rows) {
        out << "  <tr>";
        for (const auto& cell : row) {
            out << "<td>" << EscapeHtml(cell) << "</td>";
        }
        out << "</tr>\n";
    }
    out << "</table>\n";
}

void PrintLatex(const QueryResult& result, std::ostream& out) {
    int n = static_cast<int>(result.column_names.size());
    if (n == 0)
        return;
    out << "\\begin{tabular}{";
    for (int i = 0; i < n; ++i)
        out << "l";
    out << "}\n";
    for (int i = 0; i < n; ++i) {
        if (i > 0)
            out << " & ";
        out << EscapeLatex(result.column_names[i]);
    }
    out << " \\\\\n";
    out << "\\hline\n";
    for (const auto& row : result.rows) {
        for (int i = 0; i < static_cast<int>(row.size()); ++i) {
            if (i > 0)
                out << " & ";
            out << EscapeLatex(row[i]);
        }
        out << " \\\\\n";
    }
    out << "\\end{tabular}\n";
}

int PrintQueryResult(const QueryResult& result, const PrintOptions& opts, std::ostream& out) {
    if (!result.success) {
        out << "ERROR:  " << result.error_message << "\n";
        return 0;
    }
    switch (opts.format) {
        case OutputFormat::kAligned:
            PrintAligned(result, opts, out);
            break;
        case OutputFormat::kUnaligned:
            PrintUnaligned(result, opts, out);
            break;
        case OutputFormat::kCsv:
            PrintCsv(result, out);
            break;
        case OutputFormat::kJson:
            PrintJson(result, out);
            break;
        case OutputFormat::kHtml:
            PrintHtml(result, out);
            break;
        case OutputFormat::kLatex:
            PrintLatex(result, out);
            break;
    }
    return static_cast<int>(result.rows.size());
}

}  // namespace pgcpp::tools
