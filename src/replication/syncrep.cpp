// syncrep.cpp — Synchronous replication.
//
// Converted from PostgreSQL 15's src/backend/replication/syncrep.c.
// SyncRepConfigUpdate / SyncRepConfigParse manipulate the active
// SyncRepConfig; SyncRepWaitForLSN is a stub that returns immediately.
#include "replication/syncrep.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "common/error/elog.hpp"
#include "replication/walsender.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::replication {

using pgcpp::error::LogLevel;

namespace {

SyncRepConfig& Cfg() {
    static SyncRepConfig c;
    return c;
}

int& Waiters() {
    static int w = 0;
    return w;
}

std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            std::string part = Trim(s.substr(start, i - start));
            if (!part.empty()) {
                out.push_back(std::move(part));
            }
            start = i + 1;
        }
    }
    return out;
}

}  // namespace

void SyncRepConfigInit() {
    Cfg() = SyncRepConfig{};
    Cfg().num_sync = 0;  // "no sync standbys configured"
    Cfg().initialized = true;
    Waiters() = 0;
}

void SyncRepConfigReset() {
    SyncRepConfigInit();
}

bool SyncRepConfigUpdate(std::vector<std::string> standby_names, int num_sync,
                         SyncRepSyncMethod method) {
    if (num_sync < 0 || num_sync > static_cast<int>(standby_names.size())) {
        ereport(LogLevel::kError, "SyncRepConfigUpdate: num_sync out of range");
        return false;
    }
    Cfg().standby_names = std::move(standby_names);
    Cfg().num_sync = num_sync;
    Cfg().method = method;
    Cfg().initialized = true;
    return true;
}

const SyncRepConfig* SyncRepConfigGet() {
    if (!Cfg().initialized) {
        SyncRepConfigInit();
    }
    return &Cfg();
}

bool SyncRepConfigParse(const std::string& text) {
    std::string s = Trim(text);
    if (s.empty()) {
        // Empty config means "no sync standbys" (PG semantics).
        return SyncRepConfigUpdate({}, 0, SyncRepSyncMethod::kPriority);
    }

    SyncRepSyncMethod method = SyncRepSyncMethod::kPriority;
    int num_sync = 1;
    std::string names_part = s;

    // Form 1: "ANY n (a, b, c)" -> quorum, n-of-M.
    if (s.size() >= 4 && (s.substr(0, 4) == "ANY " || s.substr(0, 4) == "any ")) {
        method = SyncRepSyncMethod::kQuorum;
        // Find the count.
        std::size_t paren = s.find('(');
        if (paren == std::string::npos) {
            ereport(LogLevel::kError, "SyncRepConfigParse: missing '(' in ANY form");
            return false;
        }
        std::string num_str = Trim(s.substr(4, paren - 4));
        if (num_str.empty()) {
            ereport(LogLevel::kError, "SyncRepConfigParse: missing count in ANY form");
            return false;
        }
        try {
            num_sync = std::stoi(num_str);
        } catch (...) {
            ereport(LogLevel::kError, "SyncRepConfigParse: invalid count \"" + num_str + "\"");
            return false;
        }
        std::size_t close = s.find(')');
        if (close == std::string::npos) {
            ereport(LogLevel::kError, "SyncRepConfigParse: missing ')' in ANY form");
            return false;
        }
        names_part = s.substr(paren + 1, close - paren - 1);
    } else if (s.find('(') != std::string::npos) {
        // Form 2: "n (a, b, c)" -> priority, n-of-M.
        std::size_t paren = s.find('(');
        std::size_t close = s.find(')');
        if (close == std::string::npos) {
            ereport(LogLevel::kError, "SyncRepConfigParse: missing ')' in n-list form");
            return false;
        }
        std::string num_str = Trim(s.substr(0, paren));
        if (num_str.empty()) {
            ereport(LogLevel::kError, "SyncRepConfigParse: missing count in n-list form");
            return false;
        }
        try {
            num_sync = std::stoi(num_str);
        } catch (...) {
            ereport(LogLevel::kError, "SyncRepConfigParse: invalid count \"" + num_str + "\"");
            return false;
        }
        names_part = s.substr(paren + 1, close - paren - 1);
    }
    // Form 3: "a, b, c" -> priority, num_sync defaults to 1.

    std::vector<std::string> names = Split(names_part, ',');
    if (static_cast<int>(names.size()) < num_sync) {
        ereport(LogLevel::kError, "SyncRepConfigParse: count exceeds standby count");
        return false;
    }
    return SyncRepConfigUpdate(std::move(names), num_sync, method);
}

transaction::XLogRecPtr SyncRepWaitForLSN(transaction::XLogRecPtr lsn) {
    // PG blocks on a latch waiting for standbys to ACK; pgcpp is
    // single-process so we check once and return. We still record that
    // a waiter exists transiently so tests can observe the call shape.
    ++Waiters();
    (void)SyncRepIsSatisfied(lsn);
    --Waiters();
    return lsn;
}

int SyncRepGetWaiters() {
    return Waiters();
}

int SyncRepCountAcked(transaction::XLogRecPtr lsn) {
    const SyncRepConfig* c = SyncRepConfigGet();
    if (c->standby_names.empty() || c->num_sync == 0) {
        return 0;
    }

    WalSndCtlData* ctl = GetWalSndCtl();
    if (ctl == nullptr) {
        return 0;
    }

    int count = 0;
    for (const auto& name : c->standby_names) {
        // Find the WalSnd whose application_name matches this config entry.
        // Wildcard "*" matches any standby.
        for (const auto& s : ctl->walsenders) {
            bool match = (name == "*") || (s.application_name == name);
            if (match && s.flush_ptr >= lsn) {
                ++count;
                break;  // only count the first match per config entry
            }
        }
    }
    return count;
}

bool SyncRepIsSatisfied(transaction::XLogRecPtr lsn) {
    const SyncRepConfig* c = SyncRepConfigGet();
    if (c->num_sync == 0) {
        return true;  // no sync standbys configured
    }
    return SyncRepCountAcked(lsn) >= c->num_sync;
}

void SyncRepReleaseWaiters(transaction::XLogRecPtr /*lsn*/) {
    // In PG, this wakes backends blocked in SyncRepWaitForLSN via latches.
    // pgcpp is single-process with no blocking, so there's nothing to wake.
    // The function exists for API completeness.
}

bool SyncRepIsSyncStandby(const std::string& application_name) {
    const SyncRepConfig* c = SyncRepConfigGet();
    for (const auto& name : c->standby_names) {
        if (name == application_name || name == "*") {
            return true;
        }
    }
    return false;
}

}  // namespace pgcpp::replication
