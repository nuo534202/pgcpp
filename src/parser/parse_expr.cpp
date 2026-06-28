// parse_expr.cpp — Expression transformation for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_expr.c.
// Transforms raw parse tree expressions (A_Expr, A_Const, ColumnRef, etc.)
// into transformed expression nodes (OpExpr, Const, Var, etc.).
#include "pgcpp/parser/parse_expr.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/analyze.hpp"
#include "pgcpp/parser/parse_clause.hpp"
#include "pgcpp/parser/parse_coerce.hpp"
#include "pgcpp/parser/parse_func.hpp"
#include "pgcpp/parser/parse_oper.hpp"
#include "pgcpp/parser/parse_relation.hpp"
#include "pgcpp/parser/parse_type.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;
using pgcpp::nodes::Value;
using pgcpp::types::kBoolOid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;

static constexpr Oid kUnknownOid = 705;

// Forward declarations of internal transform functions.
static Node* transformColumnRef(ParseState* pstate, ColumnRef* cref);
static Node* transformAConst(ParseState* pstate, AConst* aconst);
static Node* transformAExpr(ParseState* pstate, AExpr* a);
static Node* transformFuncCall(ParseState* pstate, FuncCall* fn);
static Node* transformBoolExpr(ParseState* pstate, BoolExpr* b);
static Node* transformNullTest(ParseState* pstate, NullTest* n);
static Node* transformTypeCast(ParseState* pstate, TypeCast* tc);
static Node* transformCaseExpr(ParseState* pstate, CaseExpr* c);
static Node* transformSubLink(ParseState* pstate, SubLink* sublink);
static Node* transformParamRef(ParseState* pstate, ParamRef* pref);

// ---------------------------------------------------------------------------
// transformExpr — main entry point for expression transformation.
// ---------------------------------------------------------------------------

Node* transformExpr(ParseState* pstate, Node* expr, ParseExprKind exprKind) {
    if (expr == nullptr)
        return nullptr;

    ParseExprKind save_kind = pstate->p_expr_kind;
    pstate->p_expr_kind = exprKind;

    Node* result = transformExprRecurse(pstate, expr);

    pstate->p_expr_kind = save_kind;
    return result;
}

// ---------------------------------------------------------------------------
// transformExprRecurse — internal recursive transform dispatch.
// ---------------------------------------------------------------------------

Node* transformExprRecurse(ParseState* pstate, Node* expr) {
    if (expr == nullptr)
        return nullptr;

    NodeTag tag = nodeTag(expr);
    switch (tag) {
        case NodeTag::kColumnRef:
            return transformColumnRef(pstate, static_cast<ColumnRef*>(expr));
        case NodeTag::kAConst:
            return transformAConst(pstate, static_cast<AConst*>(expr));
        case NodeTag::kAExpr:
            return transformAExpr(pstate, static_cast<AExpr*>(expr));
        case NodeTag::kFuncCall:
            return transformFuncCall(pstate, static_cast<FuncCall*>(expr));
        case NodeTag::kBoolExpr:
            return transformBoolExpr(pstate, static_cast<BoolExpr*>(expr));
        case NodeTag::kNullTest:
            return transformNullTest(pstate, static_cast<NullTest*>(expr));
        case NodeTag::kTypeCast:
            return transformTypeCast(pstate, static_cast<TypeCast*>(expr));
        case NodeTag::kCaseExpr:
            return transformCaseExpr(pstate, static_cast<CaseExpr*>(expr));
        case NodeTag::kSubLink:
            return transformSubLink(pstate, static_cast<SubLink*>(expr));
        case NodeTag::kParamRef:
            return transformParamRef(pstate, static_cast<ParamRef*>(expr));

        // Already-transformed nodes: return as-is
        case NodeTag::kVar:
        case NodeTag::kConst:
        case NodeTag::kParam:
        case NodeTag::kOpExpr:
        case NodeTag::kFuncExpr:
        case NodeTag::kAggref:
        case NodeTag::kRelabelType:
        case NodeTag::kCoerceViaIO:
        case NodeTag::kScalarArrayOpExpr:
        case NodeTag::kCoerceToDomain:
            return expr;

        default:
            // For unhandled types, return as-is
            return expr;
    }
}

// ---------------------------------------------------------------------------
// transformColumnRef — resolve a column reference to a Var.
// ---------------------------------------------------------------------------

static Node* transformColumnRef(ParseState* pstate, ColumnRef* cref) {
    if (cref == nullptr || cref->fields.empty())
        return nullptr;

    int num_fields = static_cast<int>(cref->fields.size());

    // Case 1: unqualified column reference (e.g., "col")
    if (num_fields == 1) {
        Node* field0 = cref->fields[0];
        if (nodeTag(field0) == NodeTag::kString) {
            auto* v = static_cast<Value*>(field0);
            const std::string& colname = v->GetString();

            // Search the namespace for the column
            Node* var = scanNameSpaceForColumn(pstate, colname, cref->location);
            if (var != nullptr)
                return var;

            ereport(pgcpp::error::LogLevel::kError, "column does not exist");
            return nullptr;
        }
    }

    // Case 2: qualified column reference (e.g., "table.col")
    if (num_fields == 2) {
        Node* field0 = cref->fields[0];
        Node* field1 = cref->fields[1];

        if (nodeTag(field0) == NodeTag::kString && nodeTag(field1) == NodeTag::kString) {
            auto* tbl_val = static_cast<Value*>(field0);
            auto* col_val = static_cast<Value*>(field1);
            const std::string& tblname = tbl_val->GetString();
            const std::string& colname = col_val->GetString();

            // Find the RTE by table name
            int sublevels_up = 0;
            RangeTblEntry* rte = refnameRangeTblEntry(pstate, tblname.c_str(), &sublevels_up);
            if (rte == nullptr) {
                ereport(pgcpp::error::LogLevel::kError, "table does not exist");
                return nullptr;
            }

            Node* var = scanRTEForColumn(pstate, rte, colname, cref->location);
            if (var == nullptr) {
                ereport(pgcpp::error::LogLevel::kError, "column does not exist in table");
                return nullptr;
            }

            // Adjust varlevelsup for outer references
            if (sublevels_up > 0 && nodeTag(var) == NodeTag::kVar) {
                auto* v = static_cast<Var*>(var);
                v->varlevelsup = sublevels_up;
            }

            return var;
        }
    }

    // Case 3: table.* (star expansion)
    if (num_fields == 2) {
        Node* field1 = cref->fields[1];
        if (nodeTag(field1) == NodeTag::kAStar) {
            // This is a "table.*" reference — handled in target list expansion
            // Return the ColumnRef as-is for now; expandTargetList will handle it
            return cref;
        }
    }

    ereport(pgcpp::error::LogLevel::kError, "unrecognized column reference");
    return nullptr;
}

// ---------------------------------------------------------------------------
// transformAConst — transform an A_Const into a Const.
// ---------------------------------------------------------------------------

static Node* transformAConst(ParseState* pstate, AConst* aconst) {
    return make_const(pstate, aconst);
}

// ---------------------------------------------------------------------------
// transformAExpr — transform an A_Expr into an OpExpr, BoolExpr, etc.
// ---------------------------------------------------------------------------

static Node* transformAExpr(ParseState* pstate, AExpr* a) {
    Node* lexpr = a->lexpr ? transformExprRecurse(pstate, a->lexpr) : nullptr;
    Node* rexpr = a->rexpr ? transformExprRecurse(pstate, a->rexpr) : nullptr;

    // Get the operator name
    std::string opname;
    if (!a->name.empty()) {
        for (Node* n : a->name) {
            if (nodeTag(n) == NodeTag::kString) {
                auto* v = static_cast<Value*>(n);
                if (!opname.empty())
                    opname += " ";
                opname += v->GetString();
            }
        }
    }

    switch (a->kind) {
        case AExprKind::kOp: {
            // Handle boolean operators AND/OR/NOT as BoolExpr
            if (opname == "AND" && lexpr != nullptr && rexpr != nullptr) {
                std::vector<Node*> args = {lexpr, rexpr};
                auto* b = static_cast<BoolExpr*>(pgcpp::parser::make_andclause(std::move(args)));
                b->location = a->location;
                return b;
            }
            if (opname == "OR" && lexpr != nullptr && rexpr != nullptr) {
                std::vector<Node*> args = {lexpr, rexpr};
                auto* b = static_cast<BoolExpr*>(pgcpp::parser::make_orclause(std::move(args)));
                b->location = a->location;
                return b;
            }
            if (opname == "NOT" && rexpr != nullptr) {
                auto* b = static_cast<BoolExpr*>(pgcpp::parser::make_notclause(rexpr));
                b->location = a->location;
                return b;
            }

            // Binary operator: lexpr op rexpr
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, opname, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kOpAny: {
            // expr op ANY (array)
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_scalar_array_op(pstate, opname, true, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kOpAll: {
            // expr op ALL (array)
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_scalar_array_op(pstate, opname, false, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kIn: {
            // expr IN (list/subquery) — opname is "=" for IN, "<>" for NOT IN
            if (lexpr != nullptr && rexpr != nullptr) {
                bool use_or = (opname == "=");
                return make_scalar_array_op(pstate, use_or ? "=" : "<>", use_or, lexpr, rexpr,
                                            a->location);
            }
            break;
        }
        case AExprKind::kNullif: {
            // NULLIF(a, b) => (a = b) IS NULL ? NULL : a
            // Simplified: just use the = operator
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, "=", lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kDistinct: {
            // a IS DISTINCT FROM b
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, "<>", lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kNotDistinct: {
            // a IS NOT DISTINCT FROM b
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, "=", lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kLike: {
            // expr LIKE pattern — opname is "~~" for LIKE, "!~~" for NOT LIKE
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, opname, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kIlike: {
            // ILIKE — opname is "~~*" for ILIKE, "!~~*" for NOT ILIKE
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, opname, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kBetween: {
            // expr BETWEEN low AND high => expr >= low AND expr <= high
            // rexpr is a list [low, high]
            // For simplicity, handle as two comparisons
            // This is a simplification; PostgreSQL creates an AND of two OpExprs
            break;
        }
        default:
            break;
    }

    ereport(pgcpp::error::LogLevel::kError, "unsupported expression type in transformAExpr");
    return nullptr;
}

// ---------------------------------------------------------------------------
// transformFuncCall — transform a FuncCall into a FuncExpr or Aggref.
// ---------------------------------------------------------------------------

static Node* transformFuncCall(ParseState* pstate, FuncCall* fn) {
    // Delegate to parse_func.cpp's transformFuncCall
    return pgcpp::parser::transformFuncCall(pstate, fn, fn->location);
}

// ---------------------------------------------------------------------------
// transformBoolExpr — transform AND/OR/NOT.
// ---------------------------------------------------------------------------

static Node* transformBoolExpr(ParseState* pstate, BoolExpr* b) {
    std::vector<Node*> targs;
    for (Node* arg : b->args) {
        targs.push_back(transformExprRecurse(pstate, arg));
    }

    auto* result = makeNode<BoolExpr>();
    result->boolop = b->boolop;
    result->args = targs;
    result->location = b->location;
    return result;
}

// ---------------------------------------------------------------------------
// transformNullTest — transform IS NULL / IS NOT NULL.
// ---------------------------------------------------------------------------

static Node* transformNullTest(ParseState* pstate, NullTest* n) {
    auto* result = makeNode<NullTest>();
    result->arg = n->arg ? transformExprRecurse(pstate, n->arg) : nullptr;
    result->nulltesttype = n->nulltesttype;
    result->argisrow = n->argisrow;
    result->location = n->location;
    return result;
}

// ---------------------------------------------------------------------------
// transformTypeCast — transform a type cast expression.
// ---------------------------------------------------------------------------

static Node* transformTypeCast(ParseState* pstate, TypeCast* tc) {
    Node* expr = tc->arg ? transformExprRecurse(pstate, tc->arg) : nullptr;
    if (expr == nullptr)
        return nullptr;

    // Get the target type from the TypeName
    Oid target_type = kUnknownOid;
    int target_typmod = -1;
    if (tc->type_name != nullptr) {
        // Get the type name from the TypeName's names list
        std::string type_name;
        for (Node* n : tc->type_name->names) {
            if (nodeTag(n) == NodeTag::kString) {
                auto* v = static_cast<Value*>(n);
                type_name = v->GetString();
            }
        }
        if (!type_name.empty()) {
            target_type = typenameTypeId(type_name);
            target_typmod = tc->type_name->typemod;
        }
    }

    if (target_type == kUnknownOid || target_type == kInvalidOid) {
        return expr;  // can't cast, return as-is
    }

    Oid expr_type = exprType(expr);
    return coerce_type(pstate, expr, expr_type, target_type, target_typmod,
                       CoercionContext::kExplicit, CoercionForm::kExplicit, tc->location);
}

// ---------------------------------------------------------------------------
// transformCaseExpr — transform a CASE expression.
// ---------------------------------------------------------------------------

static Node* transformCaseExpr(ParseState* pstate, CaseExpr* c) {
    auto* result = makeNode<CaseExpr>();
    result->casetype = kUnknownOid;  // will be resolved from WHEN/ELSE results
    result->casecollid = 0;
    result->arg = c->arg ? transformExprRecurse(pstate, c->arg) : nullptr;
    result->location = c->location;

    Oid common_type = kUnknownOid;

    for (Node* when_node : c->args) {
        if (nodeTag(when_node) == NodeTag::kCaseWhen) {
            auto* when = static_cast<CaseWhen*>(when_node);
            auto* new_when = makeNode<CaseWhen>();
            new_when->expr = when->expr ? transformExprRecurse(pstate, when->expr) : nullptr;
            new_when->result = when->result ? transformExprRecurse(pstate, when->result) : nullptr;
            new_when->location = when->location;
            result->args.push_back(new_when);

            // Track common type
            if (new_when->result != nullptr) {
                Oid result_type = exprType(new_when->result);
                if (common_type == kUnknownOid) {
                    common_type = result_type;
                } else {
                    std::vector<Node*> tmp = {result->defresult, new_when->result};
                    common_type = select_common_type(pstate, tmp, "CASE", nullptr);
                }
            }
        }
    }

    result->defresult = c->defresult ? transformExprRecurse(pstate, c->defresult) : nullptr;

    // Set the case type
    if (result->defresult != nullptr) {
        Oid def_type = exprType(result->defresult);
        if (common_type == kUnknownOid) {
            common_type = def_type;
        }
    }
    result->casetype = common_type;

    return result;
}

// ---------------------------------------------------------------------------
// transformSubLink — transform a subquery expression.
// ---------------------------------------------------------------------------

static Node* transformSubLink(ParseState* pstate, SubLink* sublink) {
    auto* result = makeNode<SubLink>();
    result->sublinktype = sublink->sublinktype;
    result->sublinkid = sublink->sublinkid;
    result->testexpr =
        sublink->testexpr ? transformExprRecurse(pstate, sublink->testexpr) : nullptr;
    result->opername = sublink->opername;
    result->location = sublink->location;

    // Transform the subselect — this requires analyzing the subquery
    // For now, we keep the raw parse tree as the subselect
    // The full implementation would call parse_analyze on the subselect
    if (sublink->subselect != nullptr) {
        // Check if it's already a Query (already analyzed) or a raw SelectStmt
        if (nodeTag(sublink->subselect) == NodeTag::kQuery) {
            result->subselect = sublink->subselect;
        } else {
            // Transform the subquery using a new ParseState
            ParseState* sub_pstate = make_parsestate(pstate);
            sub_pstate->p_expr_kind = ParseExprKind::kFromSubselect;
            Query* subquery = transformStmt(sub_pstate, sublink->subselect);
            free_parsestate(sub_pstate);
            result->subselect = subquery;
        }
        pstate->p_has_sub_links = true;
    }

    return result;
}

// ---------------------------------------------------------------------------
// transformParamRef — transform a parameter reference ($1, $2, etc.).
// ---------------------------------------------------------------------------

static Node* transformParamRef(ParseState* pstate, ParamRef* pref) {
    auto* param = makeNode<Param>();
    param->paramkind = ParamKind::kExtern;
    param->paramid = pref->number;
    param->paramtype = kUnknownOid;  // type will be resolved later
    param->paramtypmod = -1;
    param->paramcollid = 0;
    param->location = pref->location;
    return param;
}

}  // namespace pgcpp::parser
