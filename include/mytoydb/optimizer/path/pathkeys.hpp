// pathkeys.h — Canonical pathkeys for the optimizer.
//
// Converted from PostgreSQL 15's src/include/optimizer/paths.h (PathKey
// section) and src/backend/optimizer/path/pathkeys.c.
//
// A PathKey describes a single sort key by referencing an EquivalenceClass
// (the column/expression being sorted on) and a sort operator (the btree
// operator that defines the ordering). A list of PathKeys represents the
// complete sort order of a Path's output.
//
// Canonical pathkeys are deduplicated: if two expressions are members of the
// same EC (e.g., "a.x" and "b.y" after "a.x = b.y"), then sorting on either
// one produces an equivalent ordering. The optimizer uses this to recognize
// when an existing sort (e.g., from an index or a subplan's mergejoin) already
// satisfies a required ordering, avoiding a redundant Sort node.
//
// For MyToyDB's Task 15.15, PathKey objects are simplified:
//   - pk_eclass is a raw pointer to the EquivalenceClass (no refcount).
//   - pk_strategy is a bool (true = descending, false = ascending).
//   - pk_nulls_first is a bool (NULLS FIRST/LAST).
#pragma once

#include <vector>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/parser/primnodes.hpp"

namespace mytoydb::optimizer {

// Forward declaration — defined in path/equivclass.hpp.
struct EquivalenceClass;

// Forward declaration — defined in mytoydb/optimizer/planner.hpp.
struct PlannerInfo;

// PathKey — one sort key referencing an EquivalenceClass.
struct PathKey {
    EquivalenceClass* pk_eclass = nullptr;  // the equivalence class of the sort expr
    mytoydb::catalog::Oid pk_opno = 0;      // sort operator (a btree operator OID)
    bool pk_descending = false;             // true = DESC, false = ASC
    bool pk_nulls_first = false;            // true = NULLS FIRST, false = NULLS LAST
};

// make_canonical_pathkey — build (or find) the canonical PathKey for the
// given EC + sort operator + direction. Deduplicates against the planner's
// canonical_pathkeys list: if an equal PathKey already exists, it is returned
// instead of allocating a new one.
PathKey* make_canonical_pathkey(PlannerInfo* root, EquivalenceClass* ec, mytoydb::catalog::Oid opno,
                                bool descending, bool nulls_first);

// make_pathkey_from_var — build a canonical PathKey for a Var by looking up
// the EC that contains the Var. If no EC exists yet (the Var is not on either
// side of a join qual), a new single-member EC is created for the Var.
// Returns nullptr if the Var is null or the EC cannot be created.
PathKey* make_pathkey_from_var(PlannerInfo* root, const mytoydb::parser::Var* var,
                               mytoydb::catalog::Oid opno, bool descending, bool nulls_first);

// pathkeys_is_subset — return true if `a` is a prefix of `b` (i.e., the
// sort ordering represented by `b` satisfies the ordering required by `a`).
// Used to check whether a child path's existing ordering satisfies a parent's
// required pathkeys (e.g., for merge join).
bool pathkeys_is_subset(const std::vector<PathKey*>& a, const std::vector<PathKey*>& b);

// pathkeys_equal — strict equality on (eclass, opno, descending, nulls_first).
bool pathkeys_equal(PathKey* a, PathKey* b);

// compare_pathkeys — three-way comparison: returns -1 if a is a strict prefix
// of b (a < b), 1 if b is a strict prefix of a (a > b), 0 if equal, or 2 if
// neither is a prefix of the other (incomparable). Mirrors PG's compare_pathkeys.
int compare_pathkeys(const std::vector<PathKey*>& a, const std::vector<PathKey*>& b);

}  // namespace mytoydb::optimizer
