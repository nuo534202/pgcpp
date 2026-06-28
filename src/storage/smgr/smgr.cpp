// smgr.cpp — Storage manager relation abstraction.
//
// Converted from PostgreSQL 15's src/backend/storage/smgr/smgr.c.
//
// Manages a hash table of SmgrRelation entries keyed by RelFileNodeBackend.
// Each SmgrRelation caches open file descriptors for its forks. The actual
// file I/O is delegated to md.c functions (folded into SmgrRelationData
// methods in this C++ conversion).
//
// In PostgreSQL, smgr.c uses a switch table to support multiple storage
// managers (md, etc.). MyToyDB only implements md, so the switch is
// eliminated and md operations are called directly.

#include "pgcpp/storage/smgr.hpp"

#include <cstring>
#include <unordered_map>
#include <vector>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace mytoydb::storage {
using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::makePallocNode;

namespace {

// The smgr hash table: maps RelFileNodeBackend → SmgrRelation.
// Uses a simple linear search over a linked list (PostgreSQL uses a hash
// table; for MyToyDB's scale this is sufficient and simpler).
//
// All SmgrRelation entries are allocated in a dedicated long-lived context.
struct SmgrHashEntry {
    SmgrRelation head = nullptr;  // first in chain
};

SmgrHashEntry g_smgr_hash;

// Base directory for storage files. Defaults to "/tmp/mytoydb_data".
// Set by SetStorageBaseDir() (typically in tests or server startup).
std::string g_storage_base_dir = "/tmp/mytoydb_data";

// Hash function for RelFileNodeBackend.
struct RelFileNodeBackendHash {
    std::size_t operator()(const RelFileNodeBackend& r) const {
        // Simple combining hash; collisions are handled by linear search.
        return std::hash<uint32_t>()(r.node.spc_node) ^
               (std::hash<uint32_t>()(r.node.db_node) << 1) ^
               (std::hash<uint32_t>()(r.node.rel_node) << 2) ^ (std::hash<int>()(r.backend) << 3);
    }
};

}  // namespace

// --- Base directory management ---

void SetStorageBaseDir(const std::string& path) {
    g_storage_base_dir = path;
}

const std::string& GetStorageBaseDir() {
    return g_storage_base_dir;
}

// relpathbackend — compute the relative path for a relation fork.
// Format: base/<db_node>/<rel_node>  (main fork)
//         base/<db_node>/<rel_node>_<forkname>  (other forks)
std::string relpathbackend(RelFileNodeBackend rnode, ForkNumber fork_num) {
    std::string path = "base/";
    // For shared relations (spc_node == 0), use "global/" instead.
    // PostgreSQL uses pg_tblspc/<spc>/<db>/ for non-default tablespaces.
    // MyToyDB only supports the default tablespace for now.
    path += std::to_string(rnode.node.db_node);
    path += '/';
    path += std::to_string(rnode.node.rel_node);

    // Append fork suffix for non-main forks.
    switch (fork_num) {
        case ForkNumber::kMain:
            break;
        case ForkNumber::kFsm:
            path += "_fsm";
            break;
        case ForkNumber::kVisibilityMap:
            path += "_vm";
            break;
        case ForkNumber::kInit:
            path += "_init";
            break;
        default:
            ereport(mytoydb::error::LogLevel::kError, "invalid fork number");
            break;
    }
    return path;
}

// --- smgr hash table ---

SmgrRelation smgropen(RelFileNodeBackend rnode) {
    // Search the linked list for an existing entry.
    for (SmgrRelation s = g_smgr_hash.head; s != nullptr; s = s->smgr_next) {
        if (s->smgr_rnode == rnode) {
            return s;
        }
    }

    // Not found: create a new entry.
    auto* reln = makePallocNode<SmgrRelationData>();
    reln->smgr_rnode = rnode;

    // Insert at head of the linked list.
    reln->smgr_next = g_smgr_hash.head;
    reln->smgr_prev = nullptr;
    if (g_smgr_hash.head != nullptr) {
        g_smgr_hash.head->smgr_prev = reln;
    }
    g_smgr_hash.head = reln;

    return reln;
}

void smgrclose(SmgrRelation reln) {
    // Close all open forks.
    for (int i = 0; i < kNumForks; ++i) {
        reln->mdclose(static_cast<ForkNumber>(i));
    }

    // Remove from linked list.
    if (reln->smgr_prev != nullptr) {
        reln->smgr_prev->smgr_next = reln->smgr_next;
    } else {
        g_smgr_hash.head = reln->smgr_next;
    }
    if (reln->smgr_next != nullptr) {
        reln->smgr_next->smgr_prev = reln->smgr_prev;
    }

    destroyPallocNode(reln);
}

void smgrcloseall() {
    while (g_smgr_hash.head != nullptr) {
        smgrclose(g_smgr_hash.head);
    }
}

// --- smgr API (delegating to md) ---

void smgrcreate(SmgrRelation reln, ForkNumber fork_num, bool is_redo) {
    reln->mdcreate(fork_num, is_redo);
}

void smgrread(SmgrRelation reln, ForkNumber fork_num, BlockNumber block_num, char* buffer) {
    reln->mdread(fork_num, block_num, buffer);
}

void smgrwrite(SmgrRelation reln, ForkNumber fork_num, BlockNumber block_num, const char* buffer,
               bool skip_fsync) {
    reln->mdwrite(fork_num, block_num, buffer, skip_fsync);
}

void smgrextend(SmgrRelation reln, ForkNumber fork_num, BlockNumber block_num, const char* buffer,
                bool skip_fsync) {
    reln->mdextend(fork_num, block_num, buffer, skip_fsync);
}

BlockNumber smgrnblocks(SmgrRelation reln, ForkNumber fork_num) {
    return reln->mdnblocks(fork_num);
}

void smgrtruncate(SmgrRelation reln, ForkNumber fork_num, BlockNumber nblocks) {
    reln->mdtruncate(fork_num, nblocks);
}

void smgrimmedsync(SmgrRelation reln, ForkNumber fork_num) {
    reln->mdimmedsync(fork_num);
}

// --- M6 P0 extensions (Task 15.7.4) ---

bool smgrexists(SmgrRelation reln, ForkNumber fork_num) {
    return reln->mdexists(fork_num);
}

void smgrrelease(SmgrRelation reln) {
    reln->mdrelease();
}

void smgrreleaseall() {
    // Walk the linked list and release FDs for every open SmgrRelation,
    // keeping the entries in the hash table.
    for (SmgrRelation s = g_smgr_hash.head; s != nullptr; s = s->smgr_next) {
        s->mdrelease();
    }
}

void smgrdounlinkall(SmgrRelation reln, bool is_redo) {
    // Unlink all forks, then close and remove the SmgrRelation entry.
    for (int i = 0; i < kNumForks; ++i) {
        ForkNumber fork_num = static_cast<ForkNumber>(i);
        // Skip forks that don't exist on disk (avoids spuriously clearing
        // the segment vector for forks the relation never had).
        if (reln->mdexists(fork_num)) {
            reln->mdunlink(fork_num, is_redo);
        } else {
            // Still close any open FDs.
            reln->mdclose(fork_num);
        }
    }
    smgrclose(reln);
}

void smgrclosenode(RelFileNodeBackend rnode) {
    // Find the SmgrRelation matching rnode and close it.
    for (SmgrRelation s = g_smgr_hash.head; s != nullptr; s = s->smgr_next) {
        if (s->smgr_rnode == rnode) {
            smgrclose(s);
            return;
        }
    }
    // Not found: no-op (matches PG behavior).
}

void smgrdosyncall() {
    // MyToyDB simplification: fsync every open segment FD across all
    // SmgrRelations. PostgreSQL batches this via sync.c for checkpoint
    // efficiency; single-process MyToyDB does it directly.
    for (SmgrRelation s = g_smgr_hash.head; s != nullptr; s = s->smgr_next) {
        for (int f = 0; f < kNumForks; ++f) {
            for (const auto& entry : s->md_fd[f]) {
                if (entry.fd >= 0) {
                    fsync(entry.fd);
                }
            }
        }
    }
}

void smgrsync() {
    // Same as smgrdosyncall in MyToyDB's single-process model: iterate all
    // open FDs and fsync them. PostgreSQL distinguishes smgrsync (called at
    // checkpoint) from smgrdosyncall (called by CREATE INDEX); the
    // distinction is moot without sync.c.
    smgrdosyncall();
}

}  // namespace mytoydb::storage
