// inv_api.cpp — Large object (inversion) API.
//
// Converted from PostgreSQL 15's src/backend/storage/large_object/inv_api.c.
#include "pgcpp/storage/large_object/inv_api.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace pgcpp::storage {

namespace {

// LoMap — map of LO oid → LargeObject.
std::map<LargeObjectOid, LargeObject>& LoMap() {
    static std::map<LargeObjectOid, LargeObject> m;
    return m;
}

// NextOid — counter for picking new LO oids (PG's pg_oid_assign).
LargeObjectOid& NextOid() {
    static LargeObjectOid x = 16384;  // PG's FirstNormalObjectId
    return x;
}

}  // namespace

LargeObjectOid inv_create(LargeObjectOid oid) {
    if (oid == kInvalidLargeObjectOid) {
        // Pick the next unused oid starting from NextOid().
        while (LoMap().count(NextOid()) > 0) {
            ++NextOid();
        }
        oid = NextOid();
        ++NextOid();
    }
    LargeObject lo;
    lo.oid = oid;
    lo.owner_oid = 10;  // bootstrap superuser
    LoMap()[oid] = std::move(lo);
    return oid;
}

LargeObjectDesc* inv_open(LargeObjectOid oid, int flags) {
    auto it = LoMap().find(oid);
    if (it == LoMap().end()) {
        return nullptr;
    }
    auto* desc = new LargeObjectDesc();
    desc->oid = oid;
    desc->flags = flags;
    desc->offset = 0;
    return desc;
}

int inv_close(LargeObjectDesc* desc) {
    if (desc == nullptr) {
        return -1;
    }
    delete desc;
    return 0;
}

int inv_read(LargeObjectDesc* desc, uint8_t* buffer, int nbytes) {
    if (desc == nullptr) {
        return -1;
    }
    auto* lo = GetLargeObject(desc->oid);
    if (lo == nullptr) {
        return -1;
    }
    if (desc->offset >= lo->data.size()) {
        return 0;  // EOF
    }
    int available = static_cast<int>(lo->data.size() - desc->offset);
    int to_read = std::min(nbytes, available);
    std::memcpy(buffer, lo->data.data() + desc->offset, to_read);
    desc->offset += to_read;
    return to_read;
}

int inv_write(LargeObjectDesc* desc, const uint8_t* buffer, int nbytes) {
    if (desc == nullptr) {
        return -1;
    }
    auto* lo = GetLargeObject(desc->oid);
    if (lo == nullptr) {
        return -1;
    }
    if (desc->offset + nbytes > lo->data.size()) {
        lo->data.resize(desc->offset + nbytes, 0);
    }
    std::memcpy(lo->data.data() + desc->offset, buffer, nbytes);
    desc->offset += nbytes;
    return nbytes;
}

int64_t inv_seek(LargeObjectDesc* desc, int64_t offset, int whence) {
    if (desc == nullptr) {
        return -1;
    }
    auto* lo = GetLargeObject(desc->oid);
    if (lo == nullptr) {
        return -1;
    }
    int64_t new_offset = desc->offset;
    switch (whence) {
        case 0:  // SEEK_SET
            new_offset = offset;
            break;
        case 1:  // SEEK_CUR
            new_offset = static_cast<int64_t>(desc->offset) + offset;
            break;
        case 2:  // SEEK_END
            new_offset = static_cast<int64_t>(lo->data.size()) + offset;
            break;
        default:
            return -1;
    }
    if (new_offset < 0) {
        return -1;
    }
    desc->offset = static_cast<uint64_t>(new_offset);
    return new_offset;
}

int inv_truncate(LargeObjectDesc* desc, int64_t length) {
    if (desc == nullptr || length < 0) {
        return -1;
    }
    auto* lo = GetLargeObject(desc->oid);
    if (lo == nullptr) {
        return -1;
    }
    lo->data.resize(static_cast<std::size_t>(length), 0);
    if (desc->offset > static_cast<uint64_t>(length)) {
        desc->offset = static_cast<uint64_t>(length);
    }
    return 0;
}

int inv_drop(LargeObjectOid oid) {
    auto it = LoMap().find(oid);
    if (it == LoMap().end()) {
        return -1;
    }
    LoMap().erase(it);
    return 0;
}

void ResetLargeObjects() {
    LoMap().clear();
    NextOid() = 16384;
}

LargeObject* GetLargeObject(LargeObjectOid oid) {
    auto it = LoMap().find(oid);
    if (it == LoMap().end()) {
        return nullptr;
    }
    return &it->second;
}

int NumLargeObjects() {
    return static_cast<int>(LoMap().size());
}

int64_t inv_tell(LargeObjectDesc* desc) {
    if (desc == nullptr) {
        return -1;
    }
    return static_cast<int64_t>(desc->offset);
}

}  // namespace pgcpp::storage
