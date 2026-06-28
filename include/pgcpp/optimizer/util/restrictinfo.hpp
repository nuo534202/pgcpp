// restrictinfo.h — RestrictInfo wrapper for qual clauses.
//
// Converted from PostgreSQL 15's src/include/nodes/relation.h (RestrictInfo
// section) and src/backend/optimizer/util/restrictinfo.c.
//
// A RestrictInfo wraps a qual clause (WHERE condition) with optimizer metadata:
// which relations it references, whether it can be used for joins, and its
// estimated selectivity. The optimizer attaches RestrictInfos to RelOptInfo
// baserestrictinfo (for single-table quals) or joininfo (for join quals).
#pragma once

#include <vector>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/optimizer/path.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::optimizer {

// Forward declaration — defined in mytoydb/optimizer/planner.hpp. Forward-
// declared here so function signatures can take PlannerInfo* without pulling
// in the full planner.hpp (which would create a circular dependency, since
// planner.hpp includes path.hpp, and path.hpp is included here).
struct PlannerInfo;

struct RestrictInfo;

// EquivalenceClass — forward declaration. Defined in path/equivclass.hpp.
// Forward-declared here so RestrictInfo can hold back-pointers to the ECs
// it was derived from (used by the join planner to find merge/hash clauses).
struct EquivalenceClass;

// RestrictInfo — optimizer wrapper for a restriction clause.
//
// In PostgreSQL this is a large struct with many fields for equivalence class
// tracking, merge/hash join eligibility, etc. For MyToyDB's single-table
// workload, we keep the essential fields: the wrapped clause, the relations
// it requires, and the estimated selectivity.
//
// Task 15.15 adds mergejoin/hashjoin eligibility and EC back-pointers used by
// the join planner to find merge/hash clauses and to derive implied quals.
struct RestrictInfo {
    mytoydb::parser::Node* clause = nullptr;     // the wrapped qual expression
    Relids required_relids;                      // rels this clause references
    bool can_join = false;                       // can be used as a join qual
    bool pseudoconstant = false;                 // constant clause (no Vars)
    Selectivity norm_selec = 1.0;                // normal selectivity estimate
    mytoydb::catalog::Oid hashjoinoperator = 0;  // hash-join operator OID

    // --- Task 15.15: mergejoin/hashjoin eligibility (mirrors PG's RestrictInfo) ---
    bool mergejoinable = false;      // is the clause of the form "expr op expr"?
    bool hashjoinable = false;       // does the operator support hashing?
    mytoydb::catalog::Oid opno = 0;  // operator OID (for op family lookup)
    // Left/right EC members (set when the clause is processed by process_equivalence).
    EquivalenceClass* left_ec = nullptr;
    EquivalenceClass* right_ec = nullptr;
    // Sort operator OID for merge join (a btree operator OID).
    mytoydb::catalog::Oid mergeopfamilies = 0;
};

// make_restrictinfo — create a RestrictInfo wrapping a clause.
//
// Mirrors PG's make_restrictinfo. For MyToyDB, required_relids and
// incompatible_relids are simplified to vectors of rel indexes.
RestrictInfo* make_restrictinfo(PlannerInfo* root, mytoydb::parser::Node* clause, bool can_join,
                                bool pseudoconstant, Relids required_relids,
                                Relids incompatible_relids, mytoydb::catalog::Oid hashjoinoperator);

// make_restrictinfos_from_quals — convert a list of qual clauses into
// RestrictInfos. Each clause gets its own RestrictInfo with default metadata.
std::vector<RestrictInfo*> make_restrictinfos_from_quals(
    PlannerInfo* root, const std::vector<mytoydb::parser::Node*>& clauses);

}  // namespace mytoydb::optimizer
