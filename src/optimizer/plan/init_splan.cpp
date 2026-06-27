// init_splan.cpp — PlannerInfo / RelOptInfo initialization.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/initsplan.c.
//
// Initializes planner state for a query: builds the RTE array, constructs
// RelOptInfo for each base relation, and distributes WHERE quals. For
// MyToyDB's single-table workload, the jointree handling is simplified.
#include "mytoydb/optimizer/plan/init_splan.hpp"

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/optimizer/cost.hpp"
#include "mytoydb/optimizer/util/pathnode.hpp"
#include "mytoydb/optimizer/util/plancat.hpp"
#include "mytoydb/optimizer/util/relnode.hpp"
#include "mytoydb/optimizer/util/restrictinfo.hpp"
#include "mytoydb/parser/primnodes.hpp"

namespace mytoydb::optimizer {
using mytoydb::catalog::GetCatalog;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::BoolExpr;
using mytoydb::parser::BoolExprType;
using mytoydb::parser::FromExpr;
using mytoydb::parser::JoinExpr;
using mytoydb::parser::Node;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RangeTblRef;
using mytoydb::parser::RTEKind;
using mytoydb::parser::Var;

void build_base_rel_infos(PlannerInfo* root) {
    // Build simple_rte_array (1-based → 0-based vector slots).
    root->simple_rte_array.clear();
    root->simple_rel_array.clear();
    for (Node* node : root->parse->rtable) {
        RangeTblEntry* rte = nullptr;
        if (node != nullptr && node->GetTag() == NodeTag::kRangeTblEntry) {
            rte = static_cast<RangeTblEntry*>(node);
        }
        root->simple_rte_array.push_back(rte);
        // Pre-allocate a nullptr slot; build_simple_rel fills it on demand.
        root->simple_rel_array.push_back(nullptr);
    }
    // Build RelOptInfo for each base relation (kRelation RTEs).
    for (size_t i = 0; i < root->simple_rte_array.size(); ++i) {
        RangeTblEntry* rte = root->simple_rte_array[i];
        if (rte == nullptr || rte->rtekind != RTEKind::kRelation)
            continue;
        int relid = static_cast<int>(i + 1);  // 1-based
        RelOptInfo* rel = build_simple_rel(root, relid, nullptr);
        if (rel == nullptr)
            continue;
        // Fill catalog statistics (pages, tuples, width).
        get_relation_info(root, rte->relid, false, rel);
    }
}

void add_base_rels_to_query(PlannerInfo* root, Node* jtnode) {
    if (jtnode == nullptr)
        return;
    if (jtnode->GetTag() == NodeTag::kRangeTblRef) {
        auto* ref = static_cast<RangeTblRef*>(jtnode);
        // build_simple_rel is idempotent: returns existing RelOptInfo if present.
        build_simple_rel(root, ref->rtindex, nullptr);
        return;
    }
    if (jtnode->GetTag() == NodeTag::kFromExpr) {
        auto* from = static_cast<FromExpr*>(jtnode);
        for (Node* item : from->fromlist)
            add_base_rels_to_query(root, item);
        return;
    }
    if (jtnode->GetTag() == NodeTag::kJoinExpr) {
        auto* join = static_cast<JoinExpr*>(jtnode);
        add_base_rels_to_query(root, join->larg);
        add_base_rels_to_query(root, join->rarg);
        return;
    }
    // TODO: handle other jointree node types (subqueries, functions) as needed.
}

void deconstruct_jointree(PlannerInfo* root) {
    // Simplified: for single-table queries, the jointree is a FromExpr with
    // one RangeTblRef. add_base_rels_to_query ensures the base rel is built.
    // Multi-table JOIN deconstruction (equivalence classes, join info) is
    // left as a TODO for future phases.
    if (root->parse->jointree == nullptr)
        return;
    add_base_rels_to_query(root, root->parse->jointree);
}

// Extract the first Var's varno from a qual clause (for single-table quals).
// Returns 0 if no Var is found.
static int ExtractFirstRelid(Node* expr) {
    if (expr == nullptr)
        return 0;
    switch (expr->GetTag()) {
        case NodeTag::kVar:
            return static_cast<Var*>(expr)->varno;
        case NodeTag::kOpExpr: {
            auto* op = static_cast<mytoydb::parser::OpExpr*>(expr);
            for (Node* arg : op->args) {
                int r = ExtractFirstRelid(arg);
                if (r > 0)
                    return r;
            }
            break;
        }
        case NodeTag::kBoolExpr: {
            auto* b = static_cast<BoolExpr*>(expr);
            for (Node* arg : b->args) {
                int r = ExtractFirstRelid(arg);
                if (r > 0)
                    return r;
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

// Collect all distinct relation indexes referenced by a qual clause.
static void CollectRelids(Node* expr, Relids* relids) {
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
        case NodeTag::kOpExpr: {
            auto* op = static_cast<mytoydb::parser::OpExpr*>(expr);
            for (Node* arg : op->args)
                CollectRelids(arg, relids);
            break;
        }
        case NodeTag::kBoolExpr: {
            auto* b = static_cast<BoolExpr*>(expr);
            for (Node* arg : b->args)
                CollectRelids(arg, relids);
            break;
        }
        default:
            break;
    }
}

void distribute_quals_to_rels(PlannerInfo* root, Node* quals) {
    if (quals == nullptr)
        return;
    // Split AND'd quals into individual clauses.
    std::vector<Node*> clauses;
    if (quals->GetTag() == NodeTag::kBoolExpr) {
        auto* b = static_cast<BoolExpr*>(quals);
        if (b->boolop == BoolExprType::kAnd) {
            clauses = b->args;
        } else {
            clauses.push_back(quals);  // OR/NOT treated as a single clause
        }
    } else {
        clauses.push_back(quals);
    }
    // Wrap each clause in a RestrictInfo and attach to the referencing rel.
    auto restrictinfos = make_restrictinfos_from_quals(root, clauses);
    for (RestrictInfo* ri : restrictinfos) {
        if (ri->required_relids.empty()) {
            // Pseudoconstant clause (no Vars): attach to rel 1 as a placeholder.
            RelOptInfo* rel = find_base_rel(root, 1);
            if (rel != nullptr)
                rel->baserestrictinfo.push_back(ri);
            continue;
        }
        if (ri->required_relids.size() == 1) {
            // Single-table qual: attach to baserestrictinfo.
            int relid = ri->required_relids[0];
            RelOptInfo* rel = find_base_rel(root, relid);
            if (rel != nullptr) {
                rel->baserestrictinfo.push_back(ri);
                // Refine selectivity estimate.
                ri->norm_selec =
                    EstimateSelectivity(ri->clause, (rel->tuples > 0) ? rel->tuples : 1000);
            }
        } else {
            // Multi-table qual: attach to joininfo of the first rel (skeleton).
            int relid = ri->required_relids[0];
            RelOptInfo* rel = find_base_rel(root, relid);
            if (rel != nullptr)
                rel->joininfo.push_back(ri);
        }
    }
}

void query_planner_init(PlannerInfo* root, Query* parse) {
    root->parse = parse;
    // Build the RTE array and base RelOptInfos.
    build_base_rel_infos(root);
    // Walk the join tree to ensure all base rels are registered.
    deconstruct_jointree(root);
    // Distribute WHERE-clause quals to the appropriate rels.
    if (parse->jointree != nullptr && parse->jointree->GetTag() == NodeTag::kFromExpr) {
        Node* quals = static_cast<FromExpr*>(parse->jointree)->quals;
        distribute_quals_to_rels(root, quals);
    }
}

}  // namespace mytoydb::optimizer
