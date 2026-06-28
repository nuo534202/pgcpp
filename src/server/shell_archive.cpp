// shell_archive.cpp — Shell-based WAL segment archiving.
//
// Converted from PostgreSQL 15's src/backend/postmaster/shell_archive.c.
//
// ShellArchive executes the configured archive_command with %p (file path)
// and %f (file basename) placeholders substituted. The command is run via
// the system shell; exit status 0 means success, anything else is a failure
// that will be retried.
#include "pgcpp/server/shell_archive.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

namespace pgcpp::server {

namespace {

std::string& ArchiveCommand() {
    static std::string cmd;
    return cmd;
}

ShellArchiveStats& Stats() {
    static ShellArchiveStats s;
    return s;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Basename — return the file name component of a path.
std::string Basename(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

// Substitute — replace %p with `path` and %f with `basename(path)` in `cmd`.
std::string Substitute(const std::string& cmd, const std::string& path) {
    std::string result;
    result.reserve(cmd.size() + path.size());
    for (std::size_t i = 0; i < cmd.size(); ++i) {
        if (cmd[i] == '%' && i + 1 < cmd.size()) {
            char next = cmd[i + 1];
            if (next == 'p') {
                result += path;
                ++i;
                continue;
            }
            if (next == 'f') {
                result += Basename(path);
                ++i;
                continue;
            }
            // Unknown placeholder: keep % as-is.
        }
        result.push_back(cmd[i]);
    }
    return result;
}

}  // namespace

void InitializeShellArchive() {
    ArchiveCommand().clear();
    auto& s = Stats();
    s = ShellArchiveStats{};
}

void ResetShellArchive() {
    InitializeShellArchive();
}

void SetArchiveCommand(const std::string& cmd_template) {
    ArchiveCommand() = cmd_template;
}

std::string GetArchiveCommand() {
    return ArchiveCommand();
}

bool IsArchiveCommandSet() {
    return !ArchiveCommand().empty();
}

int ShellArchive(const std::string& file_path, const std::string& /*last*/) {
    auto& cmd = ArchiveCommand();
    if (cmd.empty()) {
        // No archive command configured: treat as success (archiving disabled).
        return 0;
    }

    std::string expanded = Substitute(cmd, file_path);
    int exit_code = std::system(expanded.c_str());

    auto& s = Stats();
    s.last_exit_code = exit_code;

    if (exit_code == 0) {
        ++s.files_archived;
        s.last_archive_time_ms = NowMs();
        s.last_archived_file = file_path;
    } else {
        ++s.failures;
    }
    return exit_code;
}

ShellArchiveStats GetShellArchiveStats() {
    return Stats();
}

}  // namespace pgcpp::server
