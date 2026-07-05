// multixact.cpp — MultiXact ID management for shared row locks.
//
// Converted from PostgreSQL 15's src/backend/access/transam/multixact.cpp.
//
// A MultiXactId identifies a set of transactions that jointly hold a lock
// on a tuple (e.g., SELECT ... FOR SHARE creates a multixact). PG stores
// multixact members in pg_multixact/members and offsets in pg_multixact/offsets.
//
// pgcpp mirrors PG's on-disk layout using two SLRUs:
//   - offsets SLRU: 4 bytes per MultiXactId (MultiXactOffset = uint32_t),
//     2048 entries per 8 KB page. The offset points to the byte position
//     (not entry index) in the members SLRU where this multixact's member
//     list begins.
//   - members SLRU: 8 bytes per member (4 bytes XID + 1 byte status + 3 pad),
//     1024 entries per 8 KB page.
//
// Member lists are NOT length-prefixed; instead, the length is implicit:
// for MultiXactId `m`, the member count = offset[m+1] - offset[m]. The
// next-offset is tracked via NextMultiXactId (which equals the highest
// allocated ID + 1).
#include "transaction/multixact.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace pgcpp::transaction {

namespace {

// SLRU control blocks. Nullptr means not initialized.
SlruCtl*& OffsetsCtl() {
    static SlruCtl* ctl = nullptr;
    return ctl;
}

SlruCtl*& MembersCtl() {
    static SlruCtl* ctl = nullptr;
    return ctl;
}

// The next MultiXactId to assign. Starts at kFirstMultiXactId.
MultiXactId& NextMultiXactIdVar() {
    static MultiXactId next = kFirstMultiXactId;
    return next;
}

// The next free byte offset in the members SLRU.
MultiXactOffset& NextMemberOffset() {
    static MultiXactOffset off = 0;
    return off;
}

// On-disk control file layout: stores next_multi and next_offset so the
// multixact allocator can resume after a restart. Lives at
// <offsets_dir>/multixact_control.
struct MultiXactControlFile {
    MultiXactId next_multi;
    MultiXactOffset next_offset;
};

std::string ControlFilePath() {
    if (OffsetsCtl() == nullptr || OffsetsCtl()->disk_dir.empty()) {
        return "";
    }
    return OffsetsCtl()->disk_dir + "/multixact_control";
}

void WriteControlFile() {
    std::string path = ControlFilePath();
    if (path.empty())
        return;

    // Create directory if needed.
    mkdir(OffsetsCtl()->disk_dir.c_str(), 0700);

    MultiXactControlFile ctrl;
    ctrl.next_multi = NextMultiXactIdVar();
    ctrl.next_offset = NextMemberOffset();

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return;
    std::size_t written = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&ctrl);
    while (written < sizeof(ctrl)) {
        ssize_t n = write(fd, p + written, sizeof(ctrl) - written);
        if (n <= 0)
            break;
        written += static_cast<std::size_t>(n);
    }
    fsync(fd);
    close(fd);
}

void ReadControlFile() {
    std::string path = ControlFilePath();
    if (path.empty())
        return;

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return;  // fresh initdb: no control file yet

    MultiXactControlFile ctrl;
    std::size_t got = 0;
    uint8_t* p = reinterpret_cast<uint8_t*>(&ctrl);
    while (got < sizeof(ctrl)) {
        ssize_t n = read(fd, p + got, sizeof(ctrl) - got);
        if (n <= 0)
            break;
        got += static_cast<std::size_t>(n);
    }
    close(fd);

    if (got == sizeof(ctrl)) {
        NextMultiXactIdVar() = ctrl.next_multi;
        NextMemberOffset() = ctrl.next_offset;
    }
}

// --- Offset SLRU helpers ---

// Read the offset stored for `multi`.
MultiXactOffset ReadOffset(MultiXactId multi) {
    if (OffsetsCtl() == nullptr)
        return 0;
    int pageno = static_cast<int>(multi / kMultiXactOffsetsPerPage);
    int entry = static_cast<int>(multi % kMultiXactOffsetsPerPage);
    int offset = entry * static_cast<int>(sizeof(MultiXactOffset));
    MultiXactOffset off = 0;
    SimpleLruRead(OffsetsCtl(), pageno, offset, &off, sizeof(off));
    return off;
}

// Write the offset for `multi`.
void WriteOffset(MultiXactId multi, MultiXactOffset off) {
    if (OffsetsCtl() == nullptr)
        return;
    int pageno = static_cast<int>(multi / kMultiXactOffsetsPerPage);
    int entry = static_cast<int>(multi % kMultiXactOffsetsPerPage);
    int offset = entry * static_cast<int>(sizeof(MultiXactOffset));
    SimpleLruWrite(OffsetsCtl(), pageno, offset, &off, sizeof(off));
}

// --- Member SLRU helpers ---
//
// Members are stored at byte position `member_byte_off` in the members SLRU.
// pageno = member_byte_off / kSlruPageSize; page_offset = member_byte_off % kSlruPageSize.
// A member entry occupies kMultiXactMemberStride bytes (xid 4 + status 1 + pad 3).

void WriteMember(MultiXactOffset member_byte_off, const MultiXactMember& m) {
    if (MembersCtl() == nullptr)
        return;
    int pageno = static_cast<int>(member_byte_off / kSlruPageSize);
    int offset = static_cast<int>(member_byte_off % kSlruPageSize);
    uint8_t buf[kMultiXactMemberStride] = {0};
    std::memcpy(buf, &m.xid, sizeof(m.xid));
    buf[4] = m.status;
    SimpleLruWrite(MembersCtl(), pageno, offset, buf, kMultiXactMemberStride);
}

MultiXactMember ReadMember(MultiXactOffset member_byte_off) {
    MultiXactMember m;
    if (MembersCtl() == nullptr)
        return m;
    int pageno = static_cast<int>(member_byte_off / kSlruPageSize);
    int offset = static_cast<int>(member_byte_off % kSlruPageSize);
    uint8_t buf[kMultiXactMemberStride] = {0};
    SimpleLruRead(MembersCtl(), pageno, offset, buf, kMultiXactMemberStride);
    std::memcpy(&m.xid, buf, sizeof(m.xid));
    m.status = buf[4];
    return m;
}

}  // namespace

void InitializeMultiXact(const std::string& offsets_dir, const std::string& members_dir) {
    if (OffsetsCtl() != nullptr) {
        SimpleLruFree(OffsetsCtl());
    }
    if (MembersCtl() != nullptr) {
        SimpleLruFree(MembersCtl());
    }
    OffsetsCtl() = SimpleLruInit("multixact_offsets", /*capacity=*/16, offsets_dir);
    MembersCtl() = SimpleLruInit("multixact_members", /*capacity=*/16, members_dir);
    // Defaults for in-memory mode (no control file).
    NextMultiXactIdVar() = kFirstMultiXactId;
    NextMemberOffset() = 0;
    // If persisted, restore the allocator counters from the control file.
    ReadControlFile();
}

void ResetMultiXact() {
    if (OffsetsCtl() != nullptr) {
        SimpleLruReset(OffsetsCtl());
    }
    if (MembersCtl() != nullptr) {
        SimpleLruReset(MembersCtl());
    }
    NextMultiXactIdVar() = kFirstMultiXactId;
    NextMemberOffset() = 0;
}

void ShutdownMultiXact() {
    if (OffsetsCtl() != nullptr) {
        SimpleLruFlush(OffsetsCtl());
    }
    if (MembersCtl() != nullptr) {
        SimpleLruFlush(MembersCtl());
    }
    // Persist the allocator counters so the next run can resume.
    WriteControlFile();
}

void FlushMultiXact() {
    if (OffsetsCtl() != nullptr) {
        SimpleLruFlush(OffsetsCtl());
    }
    if (MembersCtl() != nullptr) {
        SimpleLruFlush(MembersCtl());
    }
    // Update the control file so a crash after flush doesn't lose the
    // allocator position.
    WriteControlFile();
}

MultiXactId MultiXactIdCreate(const std::vector<MultiXactMember>& members) {
    MultiXactId multi = NextMultiXactIdVar()++;
    MultiXactOffset start = NextMemberOffset();

    // Record the start offset for this multixact.
    WriteOffset(multi, start);

    // Append each member to the members SLRU.
    for (const MultiXactMember& m : members) {
        WriteMember(NextMemberOffset(), m);
        NextMemberOffset() += kMultiXactMemberStride;
    }

    return multi;
}

MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid, uint8_t status) {
    if (!MultiXactIdIsValid(multi)) {
        return multi;
    }
    // Read existing members.
    std::vector<MultiXactMember> members = MultiXactIdGetMembers(multi);

    // If xid is already a member, no change needed.
    for (const MultiXactMember& m : members) {
        if (m.xid == xid) {
            return multi;
        }
    }

    // Otherwise, append xid and create a new MultiXactId with the expanded set.
    MultiXactMember new_member;
    new_member.xid = xid;
    new_member.status = status;
    members.push_back(new_member);
    return MultiXactIdCreate(members);
}

std::vector<MultiXactMember> MultiXactIdGetMembers(MultiXactId multi) {
    if (!MultiXactIdIsValid(multi)) {
        return {};
    }
    MultiXactOffset start = ReadOffset(multi);
    MultiXactOffset end;
    if (multi + 1 == NextMultiXactIdVar()) {
        // This is the last allocated multixact; end is the next free offset.
        end = NextMemberOffset();
    } else {
        end = ReadOffset(multi + 1);
    }

    std::vector<MultiXactMember> members;
    for (MultiXactOffset off = start; off + kMultiXactMemberStride <= end;
         off += kMultiXactMemberStride) {
        members.push_back(ReadMember(off));
    }
    return members;
}

bool MultiXactIdIsValid(MultiXactId multi) {
    return multi != kInvalidMultiXactId && multi < NextMultiXactIdVar();
}

MultiXactId GetNextMultiXactId() {
    return NextMultiXactIdVar();
}

}  // namespace pgcpp::transaction
