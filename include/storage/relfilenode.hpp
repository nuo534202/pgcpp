// relfilenode.h — Physical file identification for relations.
//
// Converted from PostgreSQL 15's src/include/storage/relfilenode.h.
// A RelFileNode identifies a relation's physical storage file by
// (tablespace, database, relation-file-node). Combined with a ForkNumber
// and BlockNumber, it uniquely addresses any page in the database.
#pragma once

#include <cstdint>

#include "catalog/catalog.hpp"  // Oid

namespace pgcpp::storage {

using pgcpp::catalog::Oid;

// ForkNumber — identifies which "fork" of a relation file to access.
// PostgreSQL stores different data in separate forks: the main fork holds
// heap/index data, the FSM holds free-space info, the visibility map
// tracks page-level visibility, and the init fork is for unlogged tables.
enum class ForkNumber : int {
    kInvalid = -1,       // InvalidForkNumber
    kMain = 0,           // MAIN_FORKNUM — heap/index data
    kFsm = 1,            // FSM_FORKNUM — free space map
    kVisibilityMap = 2,  // VISIBILITYMAP_FORKNUM — visibility map
    kInit = 3,           // INIT_FORKNUM — init for unlogged tables
};

constexpr int kMaxForkNum = 3;
constexpr int kNumForks = 4;

// RelFileNode — the physical file identifier for a relation.
// Matches PostgreSQL's RelFileNode struct exactly.
struct RelFileNode {
    Oid spc_node = 0;  // tablespace OID (pg_tablespace.oid)
    Oid db_node = 0;   // database OID (pg_database.oid)
    Oid rel_node = 0;  // relation file node (pg_class.relfilenode)

    bool operator==(const RelFileNode&) const = default;
};

// RelFileNodeBackend — RelFileNode plus the backend ID.
// Used to distinguish temporary tables belonging to different backends.
// In pgcpp (single-process), the backend field is always 0 for normal
// relations and kTempBackendId for temp tables.
struct RelFileNodeBackend {
    RelFileNode node;
    int backend = 0;  // InvalidBackendId=0, MyBackendId=1, etc.

    bool operator==(const RelFileNodeBackend&) const = default;
};

// RelFileNodeIsValid — true if the node has a valid rel_node.
inline bool RelFileNodeIsValid(const RelFileNode& rnode) {
    return rnode.rel_node != 0;
}

}  // namespace pgcpp::storage
