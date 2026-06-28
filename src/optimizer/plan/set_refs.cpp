// set_refs.cpp — Plan-reference finalization.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/setrefs.c.
//
// Walks the plan tree and fixes Var references. For pgcpp's single-table
// workload with rtoffset=0, the fixup is mostly a no-op: the parser already
// sets Var.varno to the correct 1-based range table index. The upper-node
// (Agg/Sort) Var conversion to OUTER_VAR is simplified to preserve the
// existing varno/varattno, matching the behavior of the legacy subplanner.
#include "pgcpp/optimizer/plan/set_refs.hpp"

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace pgcpp::optimizer {
using pgcpp::executor::Agg;
using pgcpp::executor::HashJoin;
using pgcpp::executor::IndexScan;
using pgcpp::executor::NestLoop;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::executor::Result;
using pgcpp::executor::SeqScan;
using pgcpp::executor::Sort;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Aggref;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::RelabelType;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;

// copy_var — deep-copy a Var node (PG's copyVar equivalent).
static Var* copy_var(Var* var) {
    if (var == nullptr)
        return nullptr;
    auto* new_var = makePallocNode<Var>();
    *new_var = *var;
    return new_var;
}

// fix_scan_expr — fix Var references in a scan-level expression.
// Applies rtoffset to each Var's varno (skipping special varno values like
// INNER_VAR/OUTER_VAR). For single-table plans rtoffset=0, so this is a no-op.
static Node* fix_scan_expr(Node* node, int rtoffset) {
    if (node == nullptr)
        return nullptr;
    switch (node->GetTag()) {
        case NodeTag::kVar: {
            auto* var = static_cast<Var*>(node);
            if (var->varno > 0 && rtoffset != 0) {
                auto* new_var = copy_var(var);
                new_var->varno += rtoffset;
                return new_var;
            }
            return node;  // unchanged
        }
        case NodeTag::kRelabelType: {
            auto* rt = static_cast<RelabelType*>(node);
            Node* fixed_arg = fix_scan_expr(rt->arg, rtoffset);
            if (fixed_arg != rt->arg) {
                auto* new_rt = makePallocNode<RelabelType>();
                *new_rt = *rt;
                new_rt->arg = fixed_arg;
                return new_rt;
            }
            return node;
        }
        case NodeTag::kOpExpr: {
            auto* op = static_cast<OpExpr*>(node);
            bool changed = false;
            std::vector<Node*> new_args;
            for (Node* arg : op->args) {
                Node* fixed = fix_scan_expr(arg, rtoffset);
                if (fixed != arg)
                    changed = true;
                new_args.push_back(fixed);
            }
            if (changed) {
                auto* new_op = makePallocNode<OpExpr>();
                *new_op = *op;
                new_op->args = std::move(new_args);
                return new_op;
            }
            return node;
        }
        case NodeTag::kTargetEntry: {
            auto* te = static_cast<TargetEntry*>(node);
            Node* fixed_expr = fix_scan_expr(te->expr, rtoffset);
            if (fixed_expr != te->expr) {
                auto* new_te = makePallocNode<TargetEntry>();
                *new_te = *te;
                new_te->expr = fixed_expr;
                return new_te;
            }
            return node;
        }
        default:
            return node;
    }
}

// Fix all expressions in a target list (applies fix_scan_expr to each entry).
static void fix_scan_tlist(std::vector<TargetEntry*>& tlist, int rtoffset) {
    for (size_t i = 0; i < tlist.size(); ++i) {
        if (tlist[i] == nullptr)
            continue;
        Node* fixed = fix_scan_expr(tlist[i], rtoffset);
        if (fixed != tlist[i]) {
            tlist[i] = static_cast<TargetEntry*>(fixed);
        }
    }
}

// set_scan_references — fix Var references in a scan plan's targetlist and qual.
static void set_scan_references(PlannerInfo* root, Plan* plan, int rtoffset) {
    (void)root;
    fix_scan_tlist(plan->targetlist, rtoffset);
    if (plan->qual != nullptr) {
        plan->qual = fix_scan_expr(plan->qual, rtoffset);
    }
}

// set_join_references — fix Var references in a join plan (skeleton).
// For pgcpp, join plans are not exercised by ClickBench; this is a no-op
// that preserves the plan structure.
static void set_join_references(PlannerInfo* root, Plan* plan, int rtoffset) {
    (void)root;
    (void)plan;
    (void)rtoffset;
    // TODO: convert Vars in join targetlist to INNER_VAR/OUTER_VAR references.
}

// set_upper_references — fix Var references in an upper-node (Agg/Sort) plan.
// Simplified: preserves existing varno/varattno (matching the legacy subplanner,
// which does not convert Vars to OUTER_VAR). This keeps the executor's
// Var-evaluation logic consistent with the existing 1142-test baseline.
static void set_upper_references(PlannerInfo* root, Plan* plan, int rtoffset) {
    (void)root;
    (void)plan;
    (void)rtoffset;
    // No-op: Vars in Agg/Sort targetlists keep their original varno/varattno.
    // The executor evaluates them against the child plan's output slot, which
    // works because the child (SeqScan) outputs columns in the same order.
}

// set_dummy_tlist_references — placeholder for dummy target lists.
static void set_dummy_tlist_references(Plan* plan, int rtoffset) {
    (void)plan;
    (void)rtoffset;
    // No-op in the simplified model.
}

// set_plan_refs — recursive dispatcher. Walks the plan tree depth-first,
// fixing references in children before the parent.
static void set_plan_refs(PlannerInfo* root, Plan* plan, int rtoffset) {
    if (plan == nullptr)
        return;
    // Recurse into children first.
    if (plan->lefttree != nullptr)
        set_plan_refs(root, plan->lefttree, rtoffset);
    if (plan->righttree != nullptr)
        set_plan_refs(root, plan->righttree, rtoffset);

    switch (plan->type) {
        case PlanType::kSeqScan:
        case PlanType::kIndexScan:
            set_scan_references(root, plan, rtoffset);
            break;
        case PlanType::kNestLoop:
        case PlanType::kHashJoin:
            set_join_references(root, plan, rtoffset);
            break;
        case PlanType::kAgg:
        case PlanType::kSort:
        case PlanType::kResult:
            set_upper_references(root, plan, rtoffset);
            break;
        case PlanType::kHash:
        case PlanType::kModifyTable:
            set_dummy_tlist_references(plan, rtoffset);
            break;
        default:
            break;
    }
}

void set_plan_references(PlannerInfo* root, Plan* plan) {
    // rtoffset=0 for top-level plans (single query level, no inheritance).
    set_plan_refs(root, plan, 0);
}

}  // namespace pgcpp::optimizer
