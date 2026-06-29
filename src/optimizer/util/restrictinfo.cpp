// restrictinfo.cpp — RestrictInfo construction and qual distribution.
//
// Converted from PostgreSQL 15's src/backend/optimizer/util/restrictinfo.c.
//
// Wraps qual clauses (WHERE conditions) in RestrictInfo structs carrying
// optimizer metadata. For pgcpp's single-table workload, the metadata is
// minimal: the wrapped clause and the relations it references.
#include "optimizer/util/restrictinfo.hpp"

#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::optimizer {
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::RelabelType;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;

// Extract the 1-based relation indexes referenced by a qual expression.
// For a single-table qual like (Var(1,1) > Const), this yields {1}.
static void ExtractRelids(Node* expr, Relids* relids) {
    if (expr == nullptr)
        return;
    switch (expr->GetTag()) {
        case NodeTag::kVar: {
            auto* var = static_cast<Var*>(expr);
            if (var->varno > 0) {  // skip special varno (INNER_VAR, etc.)
                // Deduplicate.
                for (int r : *relids) {
                    if (r == var->varno)
                        return;
                }
                relids->push_back(var->varno);
            }
            break;
        }
        case NodeTag::kRelabelType:
            ExtractRelids(static_cast<RelabelType*>(expr)->arg, relids);
            break;
        case NodeTag::kOpExpr: {
            auto* op = static_cast<OpExpr*>(expr);
            for (Node* arg : op->args)
                ExtractRelids(arg, relids);
            break;
        }
        case NodeTag::kTargetEntry:
            ExtractRelids(static_cast<TargetEntry*>(expr)->expr, relids);
            break;
        default:
            break;
    }
}

RestrictInfo* make_restrictinfo(PlannerInfo* root, Node* clause, bool can_join, bool pseudoconstant,
                                Relids required_relids, Relids incompatible_relids,
                                pgcpp::catalog::Oid hashjoinoperator) {
    (void)root;
    (void)incompatible_relids;  // not tracked in the simplified model
    auto* ri = makePallocNode<RestrictInfo>();
    ri->clause = clause;
    ri->can_join = can_join;
    ri->pseudoconstant = pseudoconstant;
    ri->required_relids = std::move(required_relids);
    ri->hashjoinoperator = hashjoinoperator;
    ri->norm_selec = 1.0;  // default; callers can refine via EstimateSelectivity
    return ri;
}

std::vector<RestrictInfo*> make_restrictinfos_from_quals(PlannerInfo* root,
                                                         const std::vector<Node*>& clauses) {
    std::vector<RestrictInfo*> result;
    for (Node* clause : clauses) {
        if (clause == nullptr)
            continue;
        Relids relids;
        ExtractRelids(clause, &relids);
        bool can_join = (relids.size() > 1);
        // incompatible_relids is empty (simplified); hashjoinoperator 0 = none.
        auto* ri = make_restrictinfo(root, clause, can_join, false, std::move(relids), {}, 0);
        result.push_back(ri);
    }
    return result;
}

}  // namespace pgcpp::optimizer
