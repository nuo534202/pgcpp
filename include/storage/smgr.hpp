// smgr.h — Storage manager relation abstraction.
//
// Converted from PostgreSQL 15's src/include/storage/smgr.h.
//
// The storage manager (smgr) provides a uniform interface for reading and
// writing relation files, decoupling the buffer manager from the specifics
// of the on-disk file layout. pgcpp implements only the "md" (magnetic
// disk) storage manager, which is PostgreSQL's default.
//
// Key design decisions (preserved from PostgreSQL):
//   - SmgrRelation caches open file descriptors per fork.
//   - Relations are addressed by RelFileNodeBackend (spc, db, rel, backend).
//   - Files are segmented: each segment holds RELSEG_SIZE blocks.
//   - smgrcreate/smgrextend/smgrread/smgrwrite/smgrnblocks/smgrtruncate
//     mirror PostgreSQL's smgr function table.
#pragma once

#include <string>
#include <vector>

#include "storage/block.hpp"
#include "storage/relfilenode.hpp"

namespace pgcpp::storage {

// SmgrRelation — a handle to an open relation's storage.
//
// In PostgreSQL, this is a struct with a switch table for multiple storage
// managers. pgcpp only has md.c, so the smgr operations are direct methods.
// The struct is palloc-allocated and lives in a long-lived memory context.
//
// File segmentation: large relations are split into segment files of
// RELSEG_SIZE blocks each. md_seg[fork] holds the file descriptors for
// each segment of fork number `fork`.
struct SmgrRelationData {
    RelFileNodeBackend smgr_rnode;  // relation physical file identifier

    // md.c private data: file descriptors per fork per segment.
    // md_fd[forkNum][segno] = file descriptor (-1 if not open).
    // We lazily open segments as they are accessed.
    struct MdfdEntry {
        int fd = -1;    // POSIX file descriptor, -1 if closed
        int segno = 0;  // segment number within this fork
    };
    std::vector<MdfdEntry> md_fd[kNumForks];

    // Doubly-linked list for the smgr hash table (next/prev in chain).
    SmgrRelationData* smgr_next = nullptr;
    SmgrRelationData* smgr_prev = nullptr;

    // --- smgr operations (delegating to md.c) ---

    // Create the storage file for a fork. Must not already exist.
    void mdcreate(ForkNumber fork_num, bool is_redo);

    // Close a fork's file descriptors (does not delete the files).
    void mdclose(ForkNumber fork_num);

    // Read a block into the provided buffer (must be BLCKSZ bytes).
    void mdread(ForkNumber fork_num, BlockNumber block_num, char* buffer);

    // Write a block from the provided buffer (must be BLCKSZ bytes).
    void mdwrite(ForkNumber fork_num, BlockNumber block_num, const char* buffer, bool skip_fsync);

    // Extend the relation by one block, writing the provided data.
    void mdextend(ForkNumber fork_num, BlockNumber block_num, const char* buffer, bool skip_fsync);

    // Return the number of blocks in the fork.
    BlockNumber mdnblocks(ForkNumber fork_num);

    // Truncate the fork to exactly nblocks blocks.
    void mdtruncate(ForkNumber fork_num, BlockNumber nblocks);

    // Flush all pending writes to disk.
    void mdimmedsync(ForkNumber fork_num);

    // --- M6 P0 extensions (Task 15.7.4) ---

    // mdunlink — unlink all segment files for the given fork. If is_redo is
    // true, missing files are silently ignored (recovery replay).
    void mdunlink(ForkNumber fork_num, bool is_redo);

    // mdexists — test whether the fork's first segment file exists on disk.
    bool mdexists(ForkNumber fork_num);

    // mdrelease — close all open FDs for this relation (all forks), keeping
    // the SmgrRelation entry. Called by smgrrelease.
    void mdrelease();

    // --- Internal helpers ---

    // Compute the file path for a given fork and segment.
    std::string mdFilePath(ForkNumber fork_num, int segno) const;

    // Open or create a segment file, returning its file descriptor.
    int mdOpenSegment(ForkNumber fork_num, int segno, bool create);

    // Ensure segment vectors are large enough for the given segment.
    void mdEnsureSegments(ForkNumber fork_num, int segno);

    // Get the file descriptor for a specific segment, opening if needed.
    int mdGetFd(ForkNumber fork_num, int segno, bool create);
};

using SmgrRelation = SmgrRelationData*;

// --- Smgr hash table and lifecycle management ---

// smgropen — return the SmgrRelation for the given RelFileNodeBackend,
// creating one if it doesn't exist yet. The relation file is NOT opened
// until a read/write/extend is issued.
SmgrRelation smgropen(RelFileNodeBackend rnode);

// smgrclose — close all file descriptors for a relation and remove it
// from the hash table.
void smgrclose(SmgrRelation reln);

// smgrcloseall — close all open relations.
void smgrcloseall();

// smgrcreate — create the main fork's storage file for a relation.
void smgrcreate(SmgrRelation reln, ForkNumber fork_num, bool is_redo);

// smgrread — read a block.
void smgrread(SmgrRelation reln, ForkNumber fork_num, BlockNumber block_num, char* buffer);

// smgrwrite — write a block.
void smgrwrite(SmgrRelation reln, ForkNumber fork_num, BlockNumber block_num, const char* buffer,
               bool skip_fsync);

// smgrextend — extend the relation by one block.
void smgrextend(SmgrRelation reln, ForkNumber fork_num, BlockNumber block_num, const char* buffer,
                bool skip_fsync);

// smgrnblocks — return the number of blocks in a fork.
BlockNumber smgrnblocks(SmgrRelation reln, ForkNumber fork_num);

// smgrtruncate — truncate a fork to nblocks blocks.
void smgrtruncate(SmgrRelation reln, ForkNumber fork_num, BlockNumber nblocks);

// smgrimmedsync — flush all writes to disk.
void smgrimmedsync(SmgrRelation reln, ForkNumber fork_num);

// --- M6 P0 extensions (Task 15.7.4) ---
//
// These functions extend the smgr API to cover the remaining P0 surface
// from PostgreSQL's smgr.c. pgcpp simplifies them for the single-process,
// no-checkpoint model.

// smgrexists — test whether the given fork's storage file exists.
bool smgrexists(SmgrRelation reln, ForkNumber fork_num);

// smgrrelease — close all open file descriptors for the relation, but keep
// the SmgrRelation entry in the hash table. Used between checkpoints to
// release OS FD pressure.
void smgrrelease(SmgrRelation reln);

// smgrreleaseall — close all open FDs for all SmgrRelations in the hash
// table. Entries remain in the table.
void smgrreleaseall();

// smgrdounlinkall — unlink all fork files for the relation (used by DROP
// TABLE post-commit cleanup). If is_redo is true, silently ignore missing
// files (recovery replay).
void smgrdounlinkall(SmgrRelation reln, bool is_redo);

// smgrclosenode — find and close the SmgrRelation matching the given
// RelFileNodeBackend, removing it from the hash table. No-op if not found.
void smgrclosenode(RelFileNodeBackend rnode);

// smgrdosyncall — fsync all dirty files for all open SmgrRelations.
// Used by CREATE INDEX / REINDEX to durably persist multiple relations
// atomically.
void smgrdosyncall();

// smgrsync — fsync all open smgr file descriptors (simplified checkpoint
// sync). pgcpp single-process: iterates all SmgrRelations and fsyncs
// every open segment FD.
void smgrsync();

// --- Base directory for storage ---
//
// In PostgreSQL, files live in $PGDATA/base/<dboid>/<relfilenode>.
// pgcpp uses a configurable base directory (default: a temp dir set by
// the test or application). This keeps the storage layer testable without
// a running PostgreSQL cluster.
void SetStorageBaseDir(const std::string& path);
const std::string& GetStorageBaseDir();

// Compute the relative path for a relation fork (without segment suffix).
// Format: base/<db_node>/<rel_node>[_forkName]
std::string relpathbackend(RelFileNodeBackend rnode, ForkNumber fork_num);

}  // namespace pgcpp::storage
