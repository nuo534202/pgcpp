// indxpath.cpp — Index path generation.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/indxpath.c.
//
// Creates IndexPath candidates for relations that have B-tree indexes
// matching the query's WHERE clause. For each index on a base relation,
// scans the relation's baserestrictinfo for indexable clauses (Var op Const)
// where the Var's varattno matches the index's first key column (indkey[0]).
// Matching clauses become index quals; the resulting IndexPath is costed
// via CostIndexScan and added to the relation's pathlist via add_path.
#include "optimizer/path/indxpath.hpp"

#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_index.hpp"
#include "common/containers/node.hpp"
#include "optimizer/cost.hpp"
#include "optimizer/path.hpp"
#include "optimizer/util/pathnode.hpp"
#include "optimizer/util/restrictinfo.hpp"
#include "parser/primnodes.hpp"

namespace pgcpp::optimizer {

using pgcpp::catalog::FormData_pg_index;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Const;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Var;

// Extract a (Var, Const) pair from an OpExpr clause of the form
// (Var op Const) or (Const op Var). Returns true on success and fills
// *var_out / *const_out. Only binary OpExprs are considered indexable.
[[maybe_unused]] static bool ExtractIndexableVarConst(const Node* clause, Var** var_out,
                                                      Const** const_out) {
    if (clause == nullptr || clause->GetTag() != NodeTag::kOpExpr)
        return false;
    const auto* op = static_cast<const OpExpr*>(clause);
    if (op->args.size() != 2)
        return false;

    Node* left = op->args[0];
    Node* right = op->args[1];
    if (left == nullptr || right == nullptr)
        return false;

    if (left->GetTag() == NodeTag::kVar && right->GetTag() == NodeTag::kConst) {
        *var_out = static_cast<Var*>(left);
        *const_out = static_cast<Const*>(right);
        return true;
    }
    if (left->GetTag() == NodeTag::kConst && right->GetTag() == NodeTag::kVar) {
        *var_out = static_cast<Var*>(right);
        *const_out = static_cast<Const*>(left);
        return true;
    }
    return false;
}

void CreateIndexPaths(PlannerInfo* root, RelOptInfo* rel) {
    if (rel == nullptr || GetCatalog() == nullptr)
        return;

    // Gather all indexable clauses from the relation's baserestrictinfo.
    // An indexable clause is an OpExpr of the form (Var op Const) where the
    // Var references this relation. Only the Var side is needed for matching.
    struct IndexableClause {
        Node* clause;  // the original OpExpr
        Var* var;      // the Var side (varno, varattno)
    };
    std::vector<IndexableClause> indexable;
    for (RestrictInfo* ri : rel->baserestrictinfo) {
        if (ri == nullptr || ri->clause == nullptr)
            continue;
        Var* var = nullptr;
        Const* con = nullptr;
        if (ExtractIndexableVarConst(ri->clause, &var, &con) && var != nullptr &&
            var->varno == rel->relindex) {
            (void)con;  // Const side not needed for matching; only varattno is used.
            indexable.push_back({ri->clause, var});
        }
    }
    if (indexable.empty())
        return;

    // Iterate over each index defined on this relation.
    auto indexes = GetCatalog()->GetIndexesByRelid(static_cast<Oid>(rel->relid));
    for (const FormData_pg_index* idx : indexes) {
        if (idx == nullptr || idx->indkey.empty())
            continue;
        // Only valid, ready indexes are usable.
        if (!idx->indisvalid || !idx->indisready || !idx->indislive)
            continue;

        // Match index's first key column against any indexable clause.
        // pg_index.indkey stores 1-based attnums for user columns.
        int16_t idx_attno = idx->indkey[0];
        if (idx_attno < 1)
            continue;

        Node* matched_clause = nullptr;
        for (const auto& ic : indexable) {
            if (ic.var->varattno == idx_attno) {
                matched_clause = ic.clause;
                break;
            }
        }
        if (matched_clause == nullptr)
            continue;

        // Build the IndexPath with the matched clause as the indexqual.
        std::vector<pgcpp::parser::Node*> indexqual = {matched_clause};
        IndexPath* ipath = create_index_path(root, rel, idx->indexrelid, indexqual);

        // Estimate selectivity for the index qual.
        int total_tuples = (rel->tuples > 0) ? rel->tuples : 1000;
        Selectivity selec = EstimateSelectivity(matched_clause, total_tuples, rel->relid);
        if (selec <= 0.0 || selec > 1.0)
            selec = 0.1;  // safety clamp

        // Cost the IndexPath.
        CostIndexScan(ipath, total_tuples, selec);

        // Add to the relation's pathlist (updates cheapest_path if cheaper).
        add_path(rel, ipath);
    }
}

}  // namespace pgcpp::optimizer
