// pg_ctl.cpp — pgcpp server control utility (pg_ctl equivalent).
//
// Converted from PostgreSQL 15's src/bin/pg_ctl/.
//
// Starts, stops, restarts, reloads, or queries the status of a pgcpp server.
//
// Usage:
//   pg_ctl -D <data_dir> start [-l log_file] [-o "-p PORT"]
//   pg_ctl -D <data_dir> stop  [-m smart|fast|immediate]
//   pg_ctl -D <data_dir> restart
//   pg_ctl -D <data_dir> reload
//   pg_ctl -D <data_dir> status
//   pg_ctl -D <data_dir> init
//   pg_ctl -D <data_dir> promote
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pgcpp/tools/pg_ctl.hpp"

using pgcpp::tools::PgCtlAction;
using pgcpp::tools::PgCtlMain;
using pgcpp::tools::PgCtlOptions;
using pgcpp::tools::PgCtlResult;
using pgcpp::tools::PgCtlStopMode;

namespace {

void PrintUsage(const char* prog_name) {
    std::fprintf(stderr, "pgcpp server control (pg_ctl equivalent)\n\n");
    std::fprintf(stderr, "Usage: %s -D <data_dir> <action> [options]\n", prog_name);
    std::fprintf(stderr, "\nActions:\n");
    std::fprintf(stderr, "  start      Start the server\n");
    std::fprintf(stderr, "  stop       Stop the server\n");
    std::fprintf(stderr, "  restart    Restart the server\n");
    std::fprintf(stderr, "  reload     Reload configuration\n");
    std::fprintf(stderr, "  status     Show server status\n");
    std::fprintf(stderr, "  init       Initialize a new cluster\n");
    std::fprintf(stderr, "  promote    Promote a standby to primary\n");
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -D <dir>    Data directory (required)\n");
    std::fprintf(stderr, "  -l <file>   Log file (start/restart only)\n");
    std::fprintf(stderr, "  -o <opts>   Server options (e.g. \"-p 5433\")\n");
    std::fprintf(stderr, "  -m <mode>   Stop mode: smart|fast|immediate (default fast)\n");
    std::fprintf(stderr, "  -W          Do not wait for operation to complete\n");
    std::fprintf(stderr, "  -s          Silent mode\n");
    std::fprintf(stderr, "  --help      Show this help\n");
}

bool ParseAction(const std::string& s, PgCtlAction* out) {
    if (s == "start") {
        *out = PgCtlAction::kStart;
    } else if (s == "stop") {
        *out = PgCtlAction::kStop;
    } else if (s == "restart") {
        *out = PgCtlAction::kRestart;
    } else if (s == "reload") {
        *out = PgCtlAction::kReload;
    } else if (s == "status") {
        *out = PgCtlAction::kStatus;
    } else if (s == "init") {
        *out = PgCtlAction::kInit;
    } else if (s == "promote") {
        *out = PgCtlAction::kPromote;
    } else if (s == "kill") {
        *out = PgCtlAction::kKill;
    } else {
        return false;
    }
    return true;
}

bool ParseStopMode(const std::string& s, PgCtlStopMode* out) {
    if (s == "smart") {
        *out = PgCtlStopMode::kSmart;
    } else if (s == "fast") {
        *out = PgCtlStopMode::kFast;
    } else if (s == "immediate") {
        *out = PgCtlStopMode::kImmediate;
    } else {
        return false;
    }
    return true;
}

// Parse a "-o \"-p PORT -h HOST\"" string and apply to opts.
void ApplyServerOptions(const std::string& opts_str, PgCtlOptions* opts) {
    // Naive whitespace-split; recognise -p and -h.
    size_t i = 0;
    while (i < opts_str.size()) {
        while (i < opts_str.size() && (opts_str[i] == ' ' || opts_str[i] == '\t')) ++i;
        if (i >= opts_str.size()) break;
        std::string tok;
        while (i < opts_str.size() && opts_str[i] != ' ' && opts_str[i] != '\t') {
            tok.push_back(opts_str[i++]);
        }
        if (tok == "-p") {
            while (i < opts_str.size() && (opts_str[i] == ' ' || opts_str[i] == '\t')) ++i;
            std::string val;
            while (i < opts_str.size() && opts_str[i] != ' ' && opts_str[i] != '\t') {
                val.push_back(opts_str[i++]);
            }
            if (!val.empty()) opts->port = std::atoi(val.c_str());
        } else if (tok == "-h") {
            while (i < opts_str.size() && (opts_str[i] == ' ' || opts_str[i] == '\t')) ++i;
            std::string val;
            while (i < opts_str.size() && opts_str[i] != ' ' && opts_str[i] != '\t') {
                val.push_back(opts_str[i++]);
            }
            if (!val.empty()) opts->listen_addr = val;
        }
    }
}

const char* ResultToString(PgCtlResult r) {
    switch (r) {
        case PgCtlResult::kOk: return "ok";
        case PgCtlResult::kInvalidDataDir: return "invalid data directory";
        case PgCtlResult::kNoPostmasterPid: return "no postmaster.pid";
        case PgCtlResult::kAlreadyRunning: return "server already running";
        case PgCtlResult::kStartFailed: return "start failed";
        case PgCtlResult::kStopFailed: return "stop failed";
        case PgCtlResult::kTimeoutExceeded: return "timeout";
        case PgCtlResult::kSignalFailed: return "signal failed";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char* argv[]) {
    PgCtlOptions opts;
    bool action_set = false;
    bool show_help = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            show_help = true;
            continue;
        }
        if (arg == "-D") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -D requires an argument\n");
                return 1;
            }
            opts.data_dir = argv[++i];
        } else if (arg == "-l") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -l requires an argument\n");
                return 1;
            }
            opts.log_file = argv[++i];
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -o requires an argument\n");
                return 1;
            }
            ApplyServerOptions(argv[++i], &opts);
        } else if (arg == "-m") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -m requires an argument\n");
                return 1;
            }
            if (!ParseStopMode(argv[++i], &opts.stop_mode)) {
                std::fprintf(stderr, "error: invalid stop mode\n");
                return 1;
            }
        } else if (arg == "-W") {
            opts.wait_secs = 0;
        } else if (arg == "-s") {
            opts.silent = true;
        } else if (arg[0] != '-' && !action_set) {
            if (!ParseAction(arg, &opts.action)) {
                std::fprintf(stderr, "error: unknown action '%s'\n", arg.c_str());
                PrintUsage(argv[0]);
                return 1;
            }
            action_set = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    if (!action_set) {
        std::fprintf(stderr, "error: no action specified\n");
        PrintUsage(argv[0]);
        return 1;
    }
    if (opts.data_dir.empty()) {
        std::fprintf(stderr, "error: data directory (-D) is required\n");
        PrintUsage(argv[0]);
        return 1;
    }

    PgCtlResult r = PgCtlMain(opts);
    if (!opts.silent) {
        std::fprintf(r == PgCtlResult::kOk ? stdout : stderr,
                     "%s\n", ResultToString(r));
    }
    return r == PgCtlResult::kOk ? 0 : 1;
}
