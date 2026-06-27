// node_funcs.cpp — implementations for the P0 subset of PG nodeFuncs.c.
//
// See node_funcs.hpp for the API contract. The walker is a simplified
// pre-order traversal: walker(node) is called first, and on true the
// traversal short-circuits. Children are then recursed in declaration
// order for the supported container node types.

#include "mytoydb/common/containers/node_funcs.hpp"

#include <utility>

#include "mytoydb/types/datum.hpp"  // kBoolOid, kInvalidOid

namespace mytoydb::nodes {

using mytoydb::parser::Aggref;
using mytoydb::parser::BooleanTest;
using mytoydb::parser::BoolExpr;
using mytoydb::parser::CaseExpr;
using mytoydb::parser::CaseWhen;
using mytoydb::parser::CoerceToDomain;
using mytoydb::parser::CoerceViaIO;
using mytoydb::parser::Const;
using mytoydb::parser::FuncExpr;
using mytoydb::parser::NullTest;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Param;
using mytoydb::parser::RangeTblRef;
using mytoydb::parser::RelabelType;
using mytoydb::parser::ScalarArrayOpExpr;
using mytoydb::parser::SubLink;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;

namespace {

constexpr Oid kInvalidOid = mytoydb::types::kInvalidOid;
constexpr Oid kBoolOid = mytoydb::types::kBoolOid;

// Walk a single child pointer; returns true if the walker short-circuited.
bool WalkOne(Node* child, const NodeWalker& walker) {
    return expression_tree_walker(child, walker);
}

// Walk a vector of child pointers; returns true on the first short-circuit.
bool WalkVec(const std::vector<Node*>& children, const NodeWalker& walker) {
    for (Node* child : children) {
        if (WalkOne(child, walker)) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// exprType / exprTypmod / exprCollation / exprLocation
// ---------------------------------------------------------------------------

Oid exprType(const Node* expr) {
    if (expr == nullptr) {
        return kInvalidOid;
    }
    switch (expr->GetTag()) {
        case NodeTag::kVar:
            return static_cast<const Var*>(expr)->vartype;
        case NodeTag::kConst:
            return static_cast<const Const*>(expr)->consttype;
        case NodeTag::kParam:
            return static_cast<const Param*>(expr)->paramtype;
        case NodeTag::kOpExpr:
            return static_cast<const OpExpr*>(expr)->opresulttype;
        case NodeTag::kFuncExpr:
            return static_cast<const FuncExpr*>(expr)->funcresulttype;
        case NodeTag::kAggref:
            return static_cast<const Aggref*>(expr)->aggtype;
        case NodeTag::kBoolExpr:
        case NodeTag::kNullTest:
        case NodeTag::kBooleanTest:
        case NodeTag::kScalarArrayOpExpr:
            return kBoolOid;
        case NodeTag::kCaseExpr:
            return static_cast<const CaseExpr*>(expr)->casetype;
        case NodeTag::kRelabelType:
            return static_cast<const RelabelType*>(expr)->resulttype;
        case NodeTag::kCoerceViaIO:
            return static_cast<const CoerceViaIO*>(expr)->resulttype;
        case NodeTag::kCoerceToDomain:
            return static_cast<const CoerceToDomain*>(expr)->resulttype;
        case NodeTag::kTargetEntry:
            return exprType(static_cast<const TargetEntry*>(expr)->expr);
        default:
            return kInvalidOid;
    }
}

int exprTypmod(const Node* expr) {
    if (expr == nullptr) {
        return -1;
    }
    switch (expr->GetTag()) {
        case NodeTag::kVar:
            return static_cast<const Var*>(expr)->vartypmod;
        case NodeTag::kConst:
            return static_cast<const Const*>(expr)->consttypmod;
        case NodeTag::kParam:
            return static_cast<const Param*>(expr)->paramtypmod;
        case NodeTag::kRelabelType:
            return static_cast<const RelabelType*>(expr)->resulttypmod;
        case NodeTag::kTargetEntry:
            return exprTypmod(static_cast<const TargetEntry*>(expr)->expr);
        default:
            return -1;
    }
}

Oid exprCollation(const Node* expr) {
    if (expr == nullptr) {
        return kInvalidOid;
    }
    switch (expr->GetTag()) {
        case NodeTag::kVar:
            return static_cast<const Var*>(expr)->varcollid;
        case NodeTag::kConst:
            return static_cast<const Const*>(expr)->constcollid;
        case NodeTag::kParam:
            return static_cast<const Param*>(expr)->paramcollid;
        case NodeTag::kOpExpr:
            return static_cast<const OpExpr*>(expr)->opcollid;
        case NodeTag::kFuncExpr:
            return static_cast<const FuncExpr*>(expr)->funccollid;
        case NodeTag::kAggref:
            return static_cast<const Aggref*>(expr)->aggcollid;
        case NodeTag::kCaseExpr:
            return static_cast<const CaseExpr*>(expr)->casecollid;
        case NodeTag::kRelabelType:
            return static_cast<const RelabelType*>(expr)->resultcollid;
        case NodeTag::kCoerceViaIO:
            return static_cast<const CoerceViaIO*>(expr)->resultcollid;
        case NodeTag::kTargetEntry:
            return exprCollation(static_cast<const TargetEntry*>(expr)->expr);
        default:
            return kInvalidOid;
    }
}

int exprLocation(const Node* expr) {
    if (expr == nullptr) {
        return -1;
    }
    switch (expr->GetTag()) {
        case NodeTag::kVar:
            return static_cast<const Var*>(expr)->location;
        case NodeTag::kConst:
            return static_cast<const Const*>(expr)->location;
        case NodeTag::kParam:
            return static_cast<const Param*>(expr)->location;
        case NodeTag::kOpExpr:
            return static_cast<const OpExpr*>(expr)->location;
        case NodeTag::kFuncExpr:
            return static_cast<const FuncExpr*>(expr)->location;
        case NodeTag::kAggref:
            return static_cast<const Aggref*>(expr)->location;
        case NodeTag::kBoolExpr:
            return static_cast<const BoolExpr*>(expr)->location;
        case NodeTag::kNullTest:
            return static_cast<const NullTest*>(expr)->location;
        case NodeTag::kBooleanTest:
            return static_cast<const BooleanTest*>(expr)->location;
        case NodeTag::kRelabelType:
            return static_cast<const RelabelType*>(expr)->location;
        case NodeTag::kCoerceViaIO:
            return static_cast<const CoerceViaIO*>(expr)->location;
        case NodeTag::kCaseExpr:
            return static_cast<const CaseExpr*>(expr)->location;
        case NodeTag::kCaseWhen:
            return static_cast<const CaseWhen*>(expr)->location;
        case NodeTag::kScalarArrayOpExpr:
            return static_cast<const ScalarArrayOpExpr*>(expr)->location;
        case NodeTag::kTargetEntry:
            // TargetEntry has no location field; delegate to the underlying
            // expression (PG behavior).
            return exprLocation(static_cast<const TargetEntry*>(expr)->expr);
        default:
            return -1;
    }
}

// ---------------------------------------------------------------------------
// expression_tree_walker
// ---------------------------------------------------------------------------

bool expression_tree_walker(Node* node, const NodeWalker& walker) {
    if (node == nullptr) {
        return false;
    }
    if (walker(node)) {
        return true;
    }
    switch (node->GetTag()) {
        case NodeTag::kOpExpr: {
            const auto* op = static_cast<OpExpr*>(node);
            return WalkVec(op->args, walker);
        }
        case NodeTag::kFuncExpr: {
            const auto* fn = static_cast<FuncExpr*>(node);
            return WalkVec(fn->args, walker);
        }
        case NodeTag::kScalarArrayOpExpr: {
            const auto* sa = static_cast<ScalarArrayOpExpr*>(node);
            return WalkVec(sa->args, walker);
        }
        case NodeTag::kBoolExpr: {
            const auto* b = static_cast<BoolExpr*>(node);
            return WalkVec(b->args, walker);
        }
        case NodeTag::kNullTest:
            return WalkOne(static_cast<NullTest*>(node)->arg, walker);
        case NodeTag::kBooleanTest:
            return WalkOne(static_cast<BooleanTest*>(node)->arg, walker);
        case NodeTag::kRelabelType:
            return WalkOne(static_cast<RelabelType*>(node)->arg, walker);
        case NodeTag::kCoerceViaIO:
            return WalkOne(static_cast<CoerceViaIO*>(node)->arg, walker);
        case NodeTag::kCoerceToDomain:
            return WalkOne(static_cast<CoerceToDomain*>(node)->arg, walker);
        case NodeTag::kTargetEntry:
            return WalkOne(static_cast<TargetEntry*>(node)->expr, walker);
        case NodeTag::kCaseExpr: {
            const auto* c = static_cast<CaseExpr*>(node);
            if (WalkOne(c->arg, walker)) {
                return true;
            }
            if (WalkVec(c->args, walker)) {
                return true;
            }
            return WalkOne(c->defresult, walker);
        }
        case NodeTag::kCaseWhen: {
            const auto* w = static_cast<CaseWhen*>(node);
            if (WalkOne(w->expr, walker)) {
                return true;
            }
            return WalkOne(w->result, walker);
        }
        case NodeTag::kSubLink: {
            const auto* s = static_cast<SubLink*>(node);
            if (WalkOne(s->testexpr, walker)) {
                return true;
            }
            return WalkOne(s->subselect, walker);
        }
        case NodeTag::kAggref: {
            const auto* a = static_cast<Aggref*>(node);
            if (WalkVec(a->aggdirectargs, walker)) {
                return true;
            }
            if (WalkVec(a->args, walker)) {
                return true;
            }
            if (WalkVec(a->aggorder, walker)) {
                return true;
            }
            if (WalkVec(a->aggdistinct, walker)) {
                return true;
            }
            return WalkOne(a->aggfilter, walker);
        }
        // Leaf nodes — nothing to recurse into.
        case NodeTag::kVar:
        case NodeTag::kConst:
        case NodeTag::kParam:
        case NodeTag::kRangeTblRef:
            return false;
        default:
            // Unsupported node type: walker already called on the node; do
            // not recurse into unknown children.
            return false;
    }
}

// ---------------------------------------------------------------------------
// Predicate queries
// ---------------------------------------------------------------------------

bool contain_aggs_of_level(Node* node, int level) {
    return expression_tree_walker(node, [level](Node* n) {
        if (n->GetTag() == NodeTag::kAggref) {
            const auto* agg = static_cast<Aggref*>(n);
            if (agg->agglevelsup == level) {
                return true;  // short-circuit
            }
        }
        return false;
    });
}

bool contain_volatile_functions(Node* node) {
    // TODO: full implementation requires looking up each FuncExpr/OpExpr's
    // underlying pg_proc.provolatile. The catalog integration for
    // provolatile lookup is not yet in place; return false conservatively
    // (callers in the planner treat this as "no volatile functions found"
    // and will re-check once the catalog hook is wired up).
    (void)node;
    return false;
}

bool contain_subplans(Node* node) {
    return expression_tree_walker(node, [](Node* n) { return n->GetTag() == NodeTag::kSubLink; });
}

}  // namespace mytoydb::nodes
