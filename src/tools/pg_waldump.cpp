// pg_waldump.cpp — WAL record dumper (pg_waldump).
//
// Reads XLogRecord entries from either the in-memory WAL buffer or a WAL
// file and prints a one-line summary per record. Optionally accumulates
// per-resource-manager statistics.
#include "tools/pg_waldump.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "transaction/xlog.hpp"

namespace pgcpp::tools {

namespace {

// RmgrEntry — pairs a resource manager ID with its human-readable name.
struct RmgrEntry {
    pgcpp::transaction::RmgrId id;
    const char* name;
};

// kRmgrTable — ordered list of resource managers matching the kRmgr*Id
// constants in xlog.hpp. The order MUST match the constant values.
constexpr std::array<RmgrEntry, 19> kRmgrTable = {{
    {pgcpp::transaction::kRmgrXlogId, "XLOG"},
    {pgcpp::transaction::kRmgrXactId, "Transaction"},
    {pgcpp::transaction::kRmgrSmgrId, "Storage"},
    {pgcpp::transaction::kRmgrDbId, "Database"},
    {pgcpp::transaction::kRmgrTblspcId, "Tablespace"},
    {pgcpp::transaction::kRmgrMultiXactId, "MultiXact"},
    {pgcpp::transaction::kRmgrRelmapId, "Relmap"},
    {pgcpp::transaction::kRmgrStandbyId, "Standby"},
    {pgcpp::transaction::kRmgrHeapId, "Heap"},
    {pgcpp::transaction::kRmgrBtreeId, "Btree"},
    {pgcpp::transaction::kRmgrHashId, "Hash"},
    {pgcpp::transaction::kRmgrGinId, "Gin"},
    {pgcpp::transaction::kRmgrGistId, "Gist"},
    {pgcpp::transaction::kRmgrSequenceId, "Sequence"},
    {pgcpp::transaction::kRmgrSpGistId, "SPGist"},
    {pgcpp::transaction::kRmgrBrinId, "BRIN"},
    {pgcpp::transaction::kRmgrCommitTsId, "CommitTs"},
    {pgcpp::transaction::kRmgrReplicationId, "ReplicationOrigin"},
    {pgcpp::transaction::kRmgrLogicalMsgId, "LogicalMsg"},
}};

// HexDigit — convert a 0-15 value to its lowercase hex character.
char HexDigit(std::uint8_t v) {
    return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10));
}

// ToHexLower — format `v` as a lowercase hex string (no leading "0x").
std::string ToHexLower(std::uint64_t v) {
    if (v == 0)
        return "0";
    std::string out;
    while (v != 0) {
        out.push_back(HexDigit(static_cast<std::uint8_t>(v & 0xF)));
        v >>= 4;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// FromHexLower — parse a single lowercase hex character to its value (0-15).
// Returns -1 on invalid input.
int FromHexLower(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

// ReadRawFromBuffer — copy `len` bytes from `buf` starting at offset `pos`
// into `out`. Returns the number of bytes actually copied (may be less than
// `len` if `pos + len` exceeds `buf.size()`).
std::size_t ReadRawFromBuffer(const std::vector<std::uint8_t>& buf, std::size_t pos, void* out,
                              std::size_t len) {
    if (pos >= buf.size())
        return 0;
    std::size_t avail = buf.size() - pos;
    std::size_t to_copy = std::min(avail, len);
    std::memcpy(out, buf.data() + pos, to_copy);
    return to_copy;
}

// ReadRawFromFile — read `len` bytes at absolute offset `pos` from `is`.
// Returns the number of bytes actually read.
std::size_t ReadRawFromFile(std::ifstream& is, std::size_t pos, void* out, std::size_t len) {
    is.clear();
    is.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    if (!is)
        return 0;
    is.read(static_cast<char*>(out), static_cast<std::streamsize>(len));
    return static_cast<std::size_t>(is.gcount());
}

// DumpOneRecord — format a single XLogRecord summary line into `out`.
void DumpOneRecord(std::ostream& out, pgcpp::transaction::XLogRecPtr lsn,
                   const pgcpp::transaction::XLogRecord& rec) {
    using namespace pgcpp::transaction;
    out << "rmgr: " << RmgrName(rec.xl_rmid)
        << " len (rec/tot): " << (rec.xl_tot_len - kSizeofXlogRecord) << "/" << rec.xl_tot_len
        << " tx: " << rec.xl_xid << " lsn: " << FormatLsn(lsn)
        << " prev: " << FormatLsn(rec.xl_prev) << " desc: info "
        << static_cast<unsigned>(rec.xl_info) << "\n";
}

}  // namespace

const char* RmgrName(pgcpp::transaction::RmgrId rmid) {
    for (const auto& e : kRmgrTable) {
        if (e.id == rmid)
            return e.name;
    }
    return "Unknown";
}

bool RmgrIdFromName(const std::string& name, pgcpp::transaction::RmgrId& out) {
    auto iequals = [](const std::string& a, const char* b) {
        if (a.size() != std::strlen(b))
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char c = a[i];
            char d = b[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (d >= 'A' && d <= 'Z')
                d = static_cast<char>(d - 'A' + 'a');
            if (c != d)
                return false;
        }
        return true;
    };
    for (const auto& e : kRmgrTable) {
        if (iequals(name, e.name)) {
            out = e.id;
            return true;
        }
    }
    return false;
}

std::string FormatLsn(pgcpp::transaction::XLogRecPtr lsn) {
    // PostgreSQL formats LSN as "XXXXXXXX/XXXXXXXX" (high 32 / low 32).
    std::uint32_t hi = static_cast<std::uint32_t>(lsn >> 32);
    std::uint32_t lo = static_cast<std::uint32_t>(lsn & 0xFFFFFFFFu);
    std::string out;
    out.reserve(17);
    out += ToHexLower(hi);
    out += '/';
    out += ToHexLower(lo);
    return out;
}

bool ParseLsn(const std::string& s, pgcpp::transaction::XLogRecPtr& out) {
    // Accept "X/Y" or a bare hex number.
    std::size_t slash = s.find('/');
    std::string hi_str;
    std::string lo_str;
    if (slash == std::string::npos) {
        hi_str = "0";
        lo_str = s;
    } else {
        hi_str = s.substr(0, slash);
        lo_str = s.substr(slash + 1);
    }
    if (hi_str.empty() || lo_str.empty())
        return false;
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    for (char c : hi_str) {
        int d = FromHexLower(c);
        if (d < 0)
            return false;
        hi = (hi << 4) | static_cast<std::uint64_t>(d);
    }
    for (char c : lo_str) {
        int d = FromHexLower(c);
        if (d < 0)
            return false;
        lo = (lo << 4) | static_cast<std::uint64_t>(d);
    }
    out = (hi << 32) | lo;
    return true;
}

WaldumpResult DumpWal(const WaldumpOptions& opts, std::ostream& out,
                      std::vector<WaldumpStats>* stats) {
    using namespace pgcpp::transaction;

    // Resolve the rmgr filter, if any.
    RmgrId filter_rmid = 0;
    bool has_filter = false;
    if (!opts.rmgr_filter.empty()) {
        if (!RmgrIdFromName(opts.rmgr_filter, filter_rmid))
            return WaldumpResult::kInvalidArgument;
        has_filter = true;
    }

    // Determine the total length of the WAL stream.
    std::size_t wal_size = 0;
    std::ifstream file;
    const bool use_file = !opts.path.empty();
    if (use_file) {
        file.open(opts.path, std::ios::binary);
        if (!file)
            return WaldumpResult::kOpenFailed;
        file.seekg(0, std::ios::end);
        auto end_pos = file.tellg();
        if (end_pos < 0)
            return WaldumpResult::kReadFailed;
        wal_size = static_cast<std::size_t>(end_pos);
    } else {
        wal_size = GetWalBufferSize();
    }

    // LSN 0 (kInvalidXLogRecPtr) is reserved as the "no LSN" sentinel and the
    // first kSizeofXlogRecord bytes of the WAL stream are a reserved page
    // header area. Treat a default start LSN of 0 as "start at the first
    // valid record", which is at byte offset kSizeofXlogRecord.
    std::size_t pos = (opts.start_lsn == 0) ? static_cast<std::size_t>(kSizeofXlogRecord)
                                            : static_cast<std::size_t>(opts.start_lsn);
    std::size_t end_pos = (opts.end_lsn == 0) ? wal_size : static_cast<std::size_t>(opts.end_lsn);
    if (pos > wal_size)
        return WaldumpResult::kOk;  // nothing to read
    if (end_pos > wal_size)
        end_pos = wal_size;
    if (pos >= end_pos)
        return WaldumpResult::kOk;

    std::size_t printed = 0;
    while (pos + kSizeofXlogRecord <= end_pos) {
        // Read the record header.
        XLogRecord rec;
        std::size_t got;
        if (use_file) {
            got = ReadRawFromFile(file, pos, &rec, sizeof(rec));
        } else {
            got = ReadRawFromBuffer(GetWalBuffer(), pos, &rec, sizeof(rec));
        }
        if (got != sizeof(rec))
            return WaldumpResult::kReadFailed;

        // Validate the record length. tot_len must include the header and
        // be at least the header size.
        if (rec.xl_tot_len < static_cast<std::uint32_t>(kSizeofXlogRecord) ||
            rec.xl_tot_len > kMaxXlogRecordLength) {
            return WaldumpResult::kReadFailed;
        }

        // Compute the LSN of this record (its byte offset in the stream).
        XLogRecPtr lsn = static_cast<XLogRecPtr>(pos);

        // Apply the rmgr filter, if any.
        if (!has_filter || rec.xl_rmid == filter_rmid) {
            if (stats != nullptr) {
                // Accumulate statistics instead of (or in addition to) printing.
                bool found = false;
                for (auto& s : *stats) {
                    if (s.rmgr_name == RmgrName(rec.xl_rmid)) {
                        s.count += 1;
                        s.total_len += rec.xl_tot_len;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    WaldumpStats s;
                    s.rmgr_name = RmgrName(rec.xl_rmid);
                    s.count = 1;
                    s.total_len = rec.xl_tot_len;
                    stats->push_back(s);
                }
            }
            if (!opts.stats) {
                DumpOneRecord(out, lsn, rec);
            }
            ++printed;
            if (opts.limit != 0 && printed >= opts.limit)
                break;
        }

        // Advance to the next record. Records are 8-byte aligned (MAXALIGN)
        // in PostgreSQL; we use a simple 8-byte alignment here.
        std::size_t next = pos + rec.xl_tot_len;
        next = (next + 7u) & ~static_cast<std::size_t>(7u);
        if (next <= pos)
            break;  // safety against malformed records
        pos = next;
    }

    // If stats mode was requested and stats were collected, print the summary.
    if (opts.stats && stats != nullptr) {
        out << "WAL statistics:\n";
        for (const auto& s : *stats) {
            out << "  " << s.rmgr_name << ": " << s.count << " records, " << s.total_len
                << " bytes\n";
        }
    }

    return WaldumpResult::kOk;
}

}  // namespace pgcpp::tools
