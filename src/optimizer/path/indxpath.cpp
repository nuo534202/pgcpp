// indxpath.cpp — Index path generation.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/indxpath.c.
//
// Creates IndexPath candidates for relations that have B-tree indexes
// matching the query's WHERE clause. For MyToyDB's ClickBench workload,
// index path generation is minimal — most queries use full table scans.
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/optimizer/cost.hpp"
#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace mytoydb::optimizer {

using mytoydb::nodes::NodeTag;
using mytoydb::parser::Node;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Var;

// Check if a qual clause can be used as an index scan qualifier.
// A usable index qual is an OpExpr of the form (Var op Const) where the
// operator is a comparison operator supported by B-tree indexes.
static bool IsIndexableClause(const Node* clause) {
    if (clause == nullptr)
        return false;
    if (clause->GetTag() != NodeTag::kOpExpr)
        return false;

    const auto* op = static_cast<const OpExpr*>(clause);
    if (op->args.size() != 2)
        return false;

    // One side must be a Var, the other a Const.
    bool left_var = (op->args[0] != nullptr && op->args[0]->GetTag() == NodeTag::kVar);
    bool right_const = (op->args[1] != nullptr && op->args[1]->GetTag() == NodeTag::kConst);
    bool left_const = (op->args[0] != nullptr && op->args[0]->GetTag() == NodeTag::kConst);
    bool right_var = (op->args[1] != nullptr && op->args[1]->GetTag() == NodeTag::kVar);

    return (left_var && right_const) || (left_const && right_var);
}

// Create index path candidates for a relation.
// Currently a stub — MyToyDB does not yet have index metadata in the catalog
// for the optimizer to use. When pg_index is populated, this function will
// generate IndexPath candidates for matching indexes.
void CreateIndexPaths(RelOptInfo* rel, const Node* qual) {
    (void)rel;
    (void)qual;
    // TODO: iterate over indexes on rel->relid, match index columns against
    // indexable clauses in qual, and create IndexPath candidates.
}

}  // namespace mytoydb::optimizer
