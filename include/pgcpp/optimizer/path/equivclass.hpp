// equivclass.h — Equivalence classes for the optimizer.
//
// Converted from PostgreSQL 15's src/include/optimizer/paths.h (EquivalenceClass
// section) and src/backend/optimizer/path/equivclass.c.
//
// An EquivalenceClass (EC) is a set of expressions known to be equal at
// runtime (e.g., the join qual "a.x = b.y" makes a.x and b.y members of a
// single EC). The optimizer uses ECs to:
//   - Derive implied join quals ("a.x = b.y" + "b.y = c.z" → "a.x = c.z").
//   - Detect mergejoinable and hashjoinable clauses.
//   - Build canonical pathkeys (sort orderings) for Sort/MergeJoin planning.
//
// For pgcpp's M10 Task 15.15, the EC machinery is simplified: a single EC
// per distinct equivalence group, no constant-propagation, no outer-join
// barriers. Each EC tracks its members, the relations it touches, and the
// mergejoinable RestrictInfos that produced it.
#pragma once

#include <vector>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/optimizer/util/restrictinfo.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace pgcpp::optimizer {

// Forward declaration — defined in pgcpp/optimizer/planner.hpp.
struct PlannerInfo;

// EquivalenceMember — one expression in an EquivalenceClass.
// In PostgreSQL this carries relids, is_const, and a datatype. For pgcpp
// we keep the expr, the relids it references, and whether it's a constant.
struct EquivalenceMember {
    pgcpp::parser::Node* expr = nullptr;  // the wrapped expression (usually a Var)
    Relids relids;                        // relations referenced (1-based RT indexes)
    bool is_const = false;                // is this a Const (no Vars)?
    pgcpp::catalog::Oid datatype = 0;     // type OID of the expression
};

// EquivalenceClass — a set of expressions known to be equal at runtime.
struct EquivalenceClass {
    std::vector<EquivalenceMember*> ec_members;  // members of the equivalence class
    Relids ec_relids;                            // union of all members' relids
    pgcpp::catalog::Oid ec_min_op = 0;           // the btree equality operator (e.g., int4eq)
    bool ec_has_const = false;                   // does the EC include a Const member?
    bool ec_below_outer_join = false;            // affected by outer-join barrier?
    bool ec_broken = false;                      // can't be used (e.g., volatile)
};

// make_restrictinfo_with_ec — decorate a RestrictInfo with mergejoin/hashjoin
// eligibility flags by consulting the catalog. Sets:
//   - mergejoinable: true if the clause is "expr op expr" with a btree op
//   - hashjoinable:  true if the operator's pg_operator.oprcanhash is true
//   - opno:          the operator OID
// Returns true if the clause is mergejoinable (and thus eligible for EC).
bool classify_restrictinfo(RestrictInfo* ri);

// process_equivalence — given a mergejoinable RestrictInfo "left = right",
// add `left` and `right` to a single EquivalenceClass (creating or merging
// existing ECs as needed) and update the RestrictInfo's left_ec/right_ec
// back-pointers. Returns true if the EC was modified, false if no change
// (e.g., the clause is not mergejoinable or the EC was already broken).
//
// Mirrors PG's process_equivalence in equivclass.c. The caller has already
// built the RestrictInfo and classified it (mergejoinable flag set). The
// clause's args[0] becomes `left` and args[1] becomes `right`.
bool process_equivalence(PlannerInfo* root, RestrictInfo* restrictinfo);

// add_eq_class — register a new EC in the planner's eq_classes list.
// Takes ownership of the EC. Does NOT deduplicate against existing ECs
// (callers should use process_equivalence for that).
void add_eq_class(PlannerInfo* root, EquivalenceClass* ec);

// find_ecs_for_rel — return all ECs that touch the given relation.
// Useful for pathkey construction (find the EC for a sort column) and
// for join path generation (find ECs shared between two relations).
std::vector<EquivalenceClass*> find_ecs_for_rel(PlannerInfo* root, int relid);

// find_ec_member_for_var — find an EquivalenceMember in any EC whose
// expression is the same Var as `var`. Returns nullptr if no match.
// Used to look up the canonical EC for a sort key (e.g., ORDER BY col).
EquivalenceMember* find_ec_member_for_var(PlannerInfo* root, const pgcpp::parser::Var* var);

// generate_join_implied_equalities — for each EC shared between `outer_rel`
// and `inner_rel`, generate a RestrictInfo equating one outer member to one
// inner member. Returns the list of implied join clauses.
//
// Mirrors PG's generate_join_implied_equalities. Used by the join planner
// to discover merge/hash clauses derived from transitive EC equality.
std::vector<RestrictInfo*> generate_join_implied_equalities(PlannerInfo* root,
                                                            RelOptInfo* outer_rel,
                                                            RelOptInfo* inner_rel);

}  // namespace pgcpp::optimizer
