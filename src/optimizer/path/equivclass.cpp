// equivclass.cpp — Equivalence class construction for the optimizer.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/equivclass.c.
//
// Builds EquivalenceClass objects from mergejoinable RestrictInfos
// (clauses of the form "expr = expr" where the operator is a btree equality
// operator). Members of a single EC are transitively equal, allowing the
// optimizer to derive implied quals and detect mergejoinable clauses across
// query subtrees.
//
// For pgcpp's Task 15.15, the EC machinery is simplified:
//   - No constant propagation (a Const member does not collapse the EC).
//   - No outer-join barrier tracking (treat all ECs as below-inner-join).
//   - No volatile-expression rejection (callers must guard).
//   - Member equality is structural (same NodeTag + same Var fields).
#include "pgcpp/optimizer/path/equivclass.hpp"

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_operator.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/optimizer/util/restrictinfo.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::optimizer {
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::RelabelType;
using pgcpp::parser::Var;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Extract the relation indexes referenced by an expression.
// Same logic as restrictinfo.cpp's ExtractRelids, but local to this module
// so we don't take a dependency on that file's static function.
void CollectRelids(Node* expr, Relids* relids) {
    if (expr == nullptr)
        return;
    switch (expr->GetTag()) {
        case NodeTag::kVar: {
            auto* var = static_cast<Var*>(expr);
            if (var->varno > 0) {
                bool found = false;
                for (int r : *relids) {
                    if (r == var->varno) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    relids->push_back(var->varno);
            }
            break;
        }
        case NodeTag::kRelabelType:
            CollectRelids(static_cast<RelabelType*>(expr)->arg, relids);
            break;
        case NodeTag::kOpExpr: {
            auto* op = static_cast<OpExpr*>(expr);
            for (Node* arg : op->args)
                CollectRelids(arg, relids);
            break;
        }
        default:
            break;
    }
}

// Determine whether an expression is a Const (for EC is_const flag).
bool IsConstExpr(Node* expr) {
    if (expr == nullptr)
        return false;
    if (expr->GetTag() == NodeTag::kConst)
        return true;
    if (expr->GetTag() == NodeTag::kRelabelType)
        return IsConstExpr(static_cast<RelabelType*>(expr)->arg);
    return false;
}

// Extract the underlying Var from a possibly-wrapped expression.
// Used to canonicalize members for structural equality comparison.
const Var* AsVar(Node* expr) {
    if (expr == nullptr)
        return nullptr;
    if (expr->GetTag() == NodeTag::kVar)
        return static_cast<Var*>(expr);
    if (expr->GetTag() == NodeTag::kRelabelType)
        return AsVar(static_cast<RelabelType*>(expr)->arg);
    return nullptr;
}

// Two members are structurally equal if they wrap the same Var
// (varno + varattno) or are the same Const value. Used to deduplicate
// EC members and to detect existing membership.
bool MembersEqual(EquivalenceMember* a, EquivalenceMember* b) {
    if (a == nullptr || b == nullptr)
        return false;
    const Var* va = AsVar(a->expr);
    const Var* vb = AsVar(b->expr);
    if (va != nullptr && vb != nullptr) {
        return va->varno == vb->varno && va->varattno == vb->varattno;
    }
    // Fall back to structural Node equality (handles Const == Const).
    if (va == nullptr && vb == nullptr && a->expr != nullptr && b->expr != nullptr) {
        return a->expr->Equals(*b->expr);
    }
    return false;
}

// Make an EquivalenceMember from an expression.
EquivalenceMember* MakeEquivalenceMember(Node* expr) {
    auto* em = makePallocNode<EquivalenceMember>();
    em->expr = expr;
    em->is_const = IsConstExpr(expr);
    CollectRelids(expr, &em->relids);
    // Derive datatype from the wrapped Var, if any.
    const Var* var = AsVar(expr);
    if (var != nullptr)
        em->datatype = var->vartype;
    return em;
}

// Add a member to an EC, deduplicating against existing members.
// Updates ec_relids and ec_has_const accordingly. Returns true if the
// member was newly added (false if already present).
bool AddMemberToEC(EquivalenceClass* ec, EquivalenceMember* em) {
    for (EquivalenceMember* existing : ec->ec_members) {
        if (MembersEqual(existing, em)) {
            return false;  // already present
        }
    }
    ec->ec_members.push_back(em);
    for (int r : em->relids) {
        bool found = false;
        for (int er : ec->ec_relids) {
            if (er == r) {
                found = true;
                break;
            }
        }
        if (!found)
            ec->ec_relids.push_back(r);
    }
    if (em->is_const)
        ec->ec_has_const = true;
    return true;
}

// Merge `src` into `dst`: copy all members from src into dst (deduplicated),
// then mark src as broken so it's no longer used standalone.
void MergeECs(EquivalenceClass* dst, EquivalenceClass* src) {
    for (EquivalenceMember* em : src->ec_members) {
        AddMemberToEC(dst, em);
    }
    src->ec_broken = true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool classify_restrictinfo(RestrictInfo* ri) {
    if (ri == nullptr || ri->clause == nullptr)
        return false;
    if (ri->clause->GetTag() != NodeTag::kOpExpr)
        return false;
    auto* op = static_cast<OpExpr*>(ri->clause);
    if (op->args.size() != 2)
        return false;

    // Look up the operator in the catalog to check merge/hash eligibility.
    auto* catalog = pgcpp::catalog::GetCatalog();
    if (catalog == nullptr)
        return false;
    const pgcpp::catalog::FormData_pg_operator* oprow = catalog->GetOperatorByOid(op->opno);
    if (oprow == nullptr)
        return false;

    ri->opno = op->opno;
    ri->mergejoinable = oprow->oprcanmerge;
    ri->hashjoinable = oprow->oprcanhash;
    if (ri->hashjoinable) {
        ri->hashjoinoperator = op->opno;
    }
    if (ri->mergejoinable) {
        ri->mergeopfamilies = op->opno;  // simplified: op OID serves as the family key
    }
    return ri->mergejoinable;
}

bool process_equivalence(PlannerInfo* root, RestrictInfo* restrictinfo) {
    if (root == nullptr || restrictinfo == nullptr)
        return false;
    if (!restrictinfo->mergejoinable)
        return false;
    if (restrictinfo->clause == nullptr || restrictinfo->clause->GetTag() != NodeTag::kOpExpr)
        return false;
    auto* op = static_cast<OpExpr*>(restrictinfo->clause);
    if (op->args.size() != 2)
        return false;

    Node* left_expr = op->args[0];
    Node* right_expr = op->args[1];

    auto* left_em = MakeEquivalenceMember(left_expr);
    auto* right_em = MakeEquivalenceMember(right_expr);

    // Find existing ECs containing `left` or `right`. There are four cases:
    //   (1) Neither side is in any existing EC: create a new EC with both.
    //   (2) Only `left` is in an existing EC: add `right` to that EC.
    //   (3) Only `right` is in an existing EC: add `left` to that EC.
    //   (4) Both sides are in existing ECs: merge them into one.
    EquivalenceClass* left_ec = nullptr;
    EquivalenceClass* right_ec = nullptr;
    for (EquivalenceClass* ec : root->eq_classes) {
        if (ec->ec_broken)
            continue;
        for (EquivalenceMember* em : ec->ec_members) {
            if (MembersEqual(em, left_em)) {
                left_ec = ec;
            }
            if (MembersEqual(em, right_em)) {
                right_ec = ec;
            }
        }
    }

    EquivalenceClass* target_ec = nullptr;
    if (left_ec != nullptr && right_ec != nullptr) {
        if (left_ec == right_ec) {
            // Both members are already in the same EC — no work to do.
            target_ec = left_ec;
        } else {
            // Merge right_ec into left_ec.
            MergeECs(left_ec, right_ec);
            target_ec = left_ec;
        }
    } else if (left_ec != nullptr) {
        AddMemberToEC(left_ec, right_em);
        target_ec = left_ec;
    } else if (right_ec != nullptr) {
        AddMemberToEC(right_ec, left_em);
        target_ec = right_ec;
    } else {
        // Create a new EC with both members.
        auto* ec = makePallocNode<EquivalenceClass>();
        ec->ec_min_op = op->opno;
        AddMemberToEC(ec, left_em);
        AddMemberToEC(ec, right_em);
        add_eq_class(root, ec);
        target_ec = ec;
    }

    // Wire back-pointers on the RestrictInfo for later lookups.
    restrictinfo->left_ec = target_ec;
    restrictinfo->right_ec = target_ec;
    return true;
}

void add_eq_class(PlannerInfo* root, EquivalenceClass* ec) {
    if (root == nullptr || ec == nullptr)
        return;
    root->eq_classes.push_back(ec);
}

std::vector<EquivalenceClass*> find_ecs_for_rel(PlannerInfo* root, int relid) {
    std::vector<EquivalenceClass*> result;
    if (root == nullptr)
        return result;
    for (EquivalenceClass* ec : root->eq_classes) {
        if (ec->ec_broken)
            continue;
        for (int r : ec->ec_relids) {
            if (r == relid) {
                result.push_back(ec);
                break;
            }
        }
    }
    return result;
}

EquivalenceMember* find_ec_member_for_var(PlannerInfo* root, const Var* var) {
    if (root == nullptr || var == nullptr)
        return nullptr;
    for (EquivalenceClass* ec : root->eq_classes) {
        if (ec->ec_broken)
            continue;
        for (EquivalenceMember* em : ec->ec_members) {
            const Var* em_var = AsVar(em->expr);
            if (em_var != nullptr && em_var->varno == var->varno &&
                em_var->varattno == var->varattno) {
                return em;
            }
        }
    }
    return nullptr;
}

std::vector<RestrictInfo*> generate_join_implied_equalities(PlannerInfo* root,
                                                            RelOptInfo* outer_rel,
                                                            RelOptInfo* inner_rel) {
    std::vector<RestrictInfo*> result;
    if (root == nullptr || outer_rel == nullptr || inner_rel == nullptr)
        return result;

    // For each EC, find one member touching outer_rel and one touching
    // inner_rel. If both exist, synthesize a join clause "outer = inner".
    for (EquivalenceClass* ec : root->eq_classes) {
        if (ec->ec_broken || ec->ec_members.size() < 2)
            continue;
        EquivalenceMember* outer_em = nullptr;
        EquivalenceMember* inner_em = nullptr;
        for (EquivalenceMember* em : ec->ec_members) {
            bool touches_outer = false;
            bool touches_inner = false;
            for (int r : em->relids) {
                if (r == outer_rel->relindex)
                    touches_outer = true;
                if (r == inner_rel->relindex)
                    touches_inner = true;
            }
            if (touches_outer && outer_em == nullptr)
                outer_em = em;
            if (touches_inner && inner_em == nullptr)
                inner_em = em;
        }
        if (outer_em == nullptr || inner_em == nullptr)
            continue;

        // Synthesize an OpExpr "outer_em.expr = inner_em.expr".
        auto* op = makePallocNode<OpExpr>();
        op->opno = ec->ec_min_op;
        op->opresulttype = pgcpp::types::kBoolOid;
        op->args.push_back(outer_em->expr);
        op->args.push_back(inner_em->expr);

        Relids relids = {outer_rel->relindex, inner_rel->relindex};
        auto* ri = make_restrictinfo(root, op, /*can_join=*/true,
                                     /*pseudoconstant=*/false, relids, {}, ec->ec_min_op);
        classify_restrictinfo(ri);
        ri->left_ec = ec;
        ri->right_ec = ec;
        result.push_back(ri);
    }
    return result;
}

}  // namespace pgcpp::optimizer
