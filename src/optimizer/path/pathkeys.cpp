// pathkeys.cpp — Canonical pathkey construction and comparison.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/pathkeys.c.
//
// Builds canonical PathKey objects (deduplicated against the planner's
// canonical_pathkeys list) and provides ordering comparison helpers used by
// the join planner to detect when a child's sort order satisfies a parent's
// required ordering.
//
// For MyToyDB's Task 15.15, the pathkey machinery is simplified:
//   - PathKey equality is by (eclass pointer, opno, descending, nulls_first).
//   - No EC constancy tracking (we trust the EC pointer identity).
//   - No subquery pathkey pushdown (subqueries are planned independently).
#include "pgcpp/optimizer/path/pathkeys.hpp"

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/optimizer/path/equivclass.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/types/datum.hpp"

namespace mytoydb::optimizer {
using mytoydb::nodes::makePallocNode;
using mytoydb::parser::Var;

PathKey* make_canonical_pathkey(PlannerInfo* root, EquivalenceClass* ec, mytoydb::catalog::Oid opno,
                                bool descending, bool nulls_first) {
    if (root == nullptr || ec == nullptr)
        return nullptr;
    // Deduplicate against existing canonical pathkeys.
    for (PathKey* existing : root->canonical_pathkeys) {
        if (existing->pk_eclass == ec && existing->pk_opno == opno &&
            existing->pk_descending == descending && existing->pk_nulls_first == nulls_first) {
            return existing;
        }
    }
    auto* pk = makePallocNode<PathKey>();
    pk->pk_eclass = ec;
    pk->pk_opno = opno;
    pk->pk_descending = descending;
    pk->pk_nulls_first = nulls_first;
    root->canonical_pathkeys.push_back(pk);
    return pk;
}

PathKey* make_pathkey_from_var(PlannerInfo* root, const Var* var, mytoydb::catalog::Oid opno,
                               bool descending, bool nulls_first) {
    if (root == nullptr || var == nullptr)
        return nullptr;
    // Look up the EC for this Var (one that contains a member equal to var).
    EquivalenceMember* em = find_ec_member_for_var(root, var);
    if (em == nullptr) {
        // No existing EC contains this Var. Create a single-member EC for it
        // by synthesizing a trivial self-equality qual "var = var" and running
        // it through process_equivalence. This keeps EC creation uniform.
        auto* self_op = makePallocNode<mytoydb::parser::OpExpr>();
        self_op->opno = opno;
        self_op->opresulttype = mytoydb::types::kBoolOid;
        auto* var_copy1 = makePallocNode<Var>();
        *var_copy1 = *var;
        auto* var_copy2 = makePallocNode<Var>();
        *var_copy2 = *var;
        self_op->args.push_back(var_copy1);
        self_op->args.push_back(var_copy2);

        Relids relids = {var->varno};
        auto* ri = make_restrictinfo(root, self_op, /*can_join=*/false,
                                     /*pseudoconstant=*/false, relids, {}, opno);
        classify_restrictinfo(ri);
        process_equivalence(root, ri);
        em = find_ec_member_for_var(root, var);
        if (em == nullptr)
            return nullptr;
    }
    // The EC's pointer is enough for pathkey identity (canonical deduplication
    // happens inside make_canonical_pathkey).
    EquivalenceClass* ec = nullptr;
    for (EquivalenceClass* candidate : root->eq_classes) {
        if (candidate->ec_broken)
            continue;
        for (EquivalenceMember* m : candidate->ec_members) {
            if (m == em) {
                ec = candidate;
                break;
            }
        }
        if (ec != nullptr)
            break;
    }
    if (ec == nullptr)
        return nullptr;
    return make_canonical_pathkey(root, ec, opno, descending, nulls_first);
}

bool pathkeys_equal(PathKey* a, PathKey* b) {
    if (a == nullptr || b == nullptr)
        return a == b;
    return a->pk_eclass == b->pk_eclass && a->pk_opno == b->pk_opno &&
           a->pk_descending == b->pk_descending && a->pk_nulls_first == b->pk_nulls_first;
}

bool pathkeys_is_subset(const std::vector<PathKey*>& a, const std::vector<PathKey*>& b) {
    // a is a prefix of b iff a.size() <= b.size() and the first a.size()
    // elements are equal.
    if (a.size() > b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!pathkeys_equal(a[i], b[i]))
            return false;
    }
    return true;
}

int compare_pathkeys(const std::vector<PathKey*>& a, const std::vector<PathKey*>& b) {
    if (pathkeys_is_subset(a, b)) {
        return (a.size() == b.size()) ? 0 : -1;
    }
    if (pathkeys_is_subset(b, a)) {
        return 1;
    }
    return 2;  // incomparable
}

}  // namespace mytoydb::optimizer
