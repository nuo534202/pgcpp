// parse_oper.cpp — Operator lookup functions for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_oper.c.
//
// Implements catalog-driven operator lookup and OpExpr construction,
// preserving PostgreSQL's resolution algorithm:
//   1. Exact match on (name, left_type, right_type) via pg_operator.
//   2. Unknown-type substitution: if one side is unknown, try the known
//      side's type for both arguments.
//   3. Candidate-based resolution: among all operators with the given
//      name, select the best match using binary-coercibility and
//      preferred-type rules.
//   4. Common-type coercion: if no match, coerce both operands to a
//      common type and retry.
//
// All operator metadata is read from the Catalog (pg_operator rows
// populated by BootstrapCatalog). No hardcoded operator table remains.

#include "pgcpp/parser/parse_oper.hpp"

#include <string>
#include <vector>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_operator.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/parse_coerce.hpp"
#include "pgcpp/parser/parse_type.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_operator;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;

// UNKNOWNOID — PostgreSQL's OID for the "unknown" pseudo-type (705).
static constexpr Oid kUnknownOid = 705;

namespace {

// OpernameGetOprid — look up an operator by name and exact operand types.
//
// Mirrors PostgreSQL's OpernameGetOprid (src/backend/parser/parse_oper.c).
// Returns the pg_operator row, or nullptr if no exact match exists.
// pgcpp has a single namespace for built-ins, so the namespace search
// in the original is omitted.
const FormData_pg_operator* OpernameGetOprid(const std::string& opname, Oid left_type,
                                             Oid right_type) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return nullptr;
    return cat->GetOperator(opname, left_type, right_type);
}

// OpernameGetCandidates — return all operators with the given name.
//
// Mirrors PostgreSQL's OpernameGetCandidates. Returns candidates in
// catalog insertion order (PostgreSQL returns them in a List; order
// matters for tie-breaking in oper_select_candidate).
std::vector<const FormData_pg_operator*> OpernameGetCandidates(const std::string& opname) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return {};
    return cat->GetOperatorsByName(opname);
}

// binary_oper_exact — find an exact or unknown-substituted operator match.
//
// Mirrors PostgreSQL's binary_oper_exact() (parse_oper.c).
// Algorithm:
//   1. Try exact match on (left_type, right_type).
//   2. If exactly one side is unknown, substitute the known side's type
//      for the unknown side and retry. This implements PostgreSQL's
//      "if one input is unknown, try the other input's type for both".
//   3. If both sides are unknown, try (text, text) — PostgreSQL resolves
//      unknown literals to text for string-category operators. For
//      arithmetic operators the caller will coerce to a common numeric
//      type via make_op's fallback.
const FormData_pg_operator* binary_oper_exact(const std::string& opname, Oid left_type,
                                              Oid right_type) {
    // Step 1: exact match.
    if (const FormData_pg_operator* op = OpernameGetOprid(opname, left_type, right_type))
        return op;

    // Step 2: one side unknown — substitute the known type.
    if (left_type == kUnknownOid && right_type != kUnknownOid) {
        if (const FormData_pg_operator* op = OpernameGetOprid(opname, right_type, right_type))
            return op;
    }
    if (right_type == kUnknownOid && left_type != kUnknownOid) {
        if (const FormData_pg_operator* op = OpernameGetOprid(opname, left_type, left_type))
            return op;
    }

    // Step 3: both unknown — try text (PostgreSQL's preferred string type).
    if (left_type == kUnknownOid && right_type == kUnknownOid) {
        if (const FormData_pg_operator* op = OpernameGetOprid(opname, kTextOid, kTextOid))
            return op;
    }

    return nullptr;
}

// IsTypePreferred — is `type_oid` a preferred type in its category?
//
// PostgreSQL uses typcategory + typispreferred (in pg_type) to resolve
// unknowns when multiple candidates exist. pgcpp's bootstrap catalog
// does not yet set these fields, so we hardcode the preferred types for
// the categories we support:
//   Numeric category: float8
//   String category:  text
//   Datetime category: timestamp
bool IsTypePreferred(Oid type_oid) {
    return type_oid == kFloat8Oid || type_oid == kTextOid || type_oid == kTimestampOid;
}

// oper_select_candidate — select the best operator from candidates.
//
// Mirrors PostgreSQL's oper_select_candidate() (parse_oper.c).
// Algorithm:
//   1. Exact match: if a candidate's arg types exactly match the inputs,
//      pick it.
//   2. Binary-coercible filter: keep candidates where both sides are
//      binary-coercible from the input types.
//   3. Preferred-type preference: if any input is unknown, prefer
//      candidates that use preferred types on the unknown side.
//   4. If still ambiguous, return the first viable candidate.
//      (PostgreSQL errors on ambiguity; pgcpp's limited type set
//      makes this rare and the fallback avoids false errors.)
const FormData_pg_operator* oper_select_candidate(
    const std::vector<const FormData_pg_operator*>& candidates, Oid left_type, Oid right_type) {
    if (candidates.empty())
        return nullptr;

    // Step 1: exact match.
    for (const auto* op : candidates) {
        if (op->oprleft == left_type && op->oprright == right_type)
            return op;
    }

    // Step 2: binary-coercible filter.
    std::vector<const FormData_pg_operator*> viable;
    for (const auto* op : candidates) {
        if (IsBinaryCoercible(left_type, op->oprleft) &&
            IsBinaryCoercible(right_type, op->oprright)) {
            viable.push_back(op);
        }
    }
    if (viable.empty())
        return nullptr;
    if (viable.size() == 1)
        return viable[0];

    // Step 3: prefer candidates using preferred types for unknown sides.
    if (left_type == kUnknownOid || right_type == kUnknownOid) {
        for (const auto* op : viable) {
            bool left_ok = (left_type != kUnknownOid) || IsTypePreferred(op->oprleft);
            bool right_ok = (right_type != kUnknownOid) || IsTypePreferred(op->oprright);
            if (left_ok && right_ok)
                return op;
        }
    }

    // Step 4: return the first viable candidate.
    return viable[0];
}

// IsOperatorShell — is this a "shell" operator (no implementation function)?
//
// PostgreSQL rejects shell operators in make_op() with
// ERRCODE_UNDEFINED_FUNCTION. A shell operator has oprcode == InvalidOid.
bool IsOperatorShell(const FormData_pg_operator* op) {
    return op->oprcode == kInvalidOid;
}

// SelectOperator — full operator selection (exact + candidate + coercion).
//
// Combines binary_oper_exact and oper_select_candidate. If no match is
// found, attempts common-type coercion and retries. Returns the selected
// operator row, or nullptr if no match exists.
//
// `ltree` and `rtree` are in/out parameters: if coercion is applied, they
// are updated to the coerced expressions.
const FormData_pg_operator* SelectOperator(ParseState* pstate, const std::string& opname,
                                           Node*& ltree, Node*& rtree, int location) {
    Oid left_type = exprType(ltree);
    Oid right_type = exprType(rtree);

    // Stage 1: exact / unknown-substituted match.
    const FormData_pg_operator* op = binary_oper_exact(opname, left_type, right_type);

    // Stage 2: candidate-based resolution.
    if (op == nullptr) {
        auto candidates = OpernameGetCandidates(opname);
        if (!candidates.empty()) {
            op = oper_select_candidate(candidates, left_type, right_type);
        }
    }

    // Stage 3: common-type coercion fallback.
    if (op == nullptr) {
        std::vector<Node*> exprs = {ltree, rtree};
        Oid common_type = select_common_type(pstate, exprs, "operator", nullptr);
        if (common_type != kInvalidOid && common_type != kUnknownOid) {
            Node* new_ltree = coerce_to_common_type(pstate, ltree, common_type, "operator");
            Node* new_rtree = coerce_to_common_type(pstate, rtree, common_type, "operator");
            left_type = common_type;
            right_type = common_type;
            op = binary_oper_exact(opname, left_type, right_type);
            if (op == nullptr) {
                auto candidates = OpernameGetCandidates(opname);
                if (!candidates.empty()) {
                    op = oper_select_candidate(candidates, left_type, right_type);
                }
            }
            if (op != nullptr) {
                ltree = new_ltree;
                rtree = new_rtree;
            }
        }
    }

    return op;
}

}  // namespace

// ---------------------------------------------------------------------------
// lookup_operator — look up a binary operator by name and operand types.
//
// Mirrors PostgreSQL's oper() function. Returns an OperatorResult with
// opno == 0 if no match is found (callers should check).
// ---------------------------------------------------------------------------

OperatorResult lookup_operator(const std::string& opname, Oid left_type, Oid right_type) {
    OperatorResult result;

    const FormData_pg_operator* op = binary_oper_exact(opname, left_type, right_type);
    if (op == nullptr) {
        auto candidates = OpernameGetCandidates(opname);
        if (!candidates.empty()) {
            op = oper_select_candidate(candidates, left_type, right_type);
        }
    }

    if (op != nullptr) {
        result.opno = op->oid;
        result.opfuncid = op->oprcode;
        result.opresulttype = op->oprresult;
        result.opretset = false;
    }

    return result;
}

// ---------------------------------------------------------------------------
// make_op — create an OpExpr node for a binary operator.
//
// Mirrors PostgreSQL's make_op() in parse_oper.c.
// Algorithm:
//   1. Reject postfix operators (pgcpp has none, so this is a no-op).
//   2. Select the operator (exact → candidate → common-type coercion).
//   3. Reject shell operators (oprcode == InvalidOid).
//   4. Build the OpExpr with the operator's metadata.
//   5. (PostgreSQL also calls enforce_generic_type_consistency for
//      polymorphic types and make_fn_arguments for coercion insertion;
//      pgcpp's coercion is handled inline by SelectOperator.)
// ---------------------------------------------------------------------------

Node* make_op(ParseState* pstate, const std::string& opname, Node* ltree, Node* rtree,
              int location) {
    // Steps 1-3: select the operator (may coerce ltree/rtree).
    const FormData_pg_operator* op = SelectOperator(pstate, opname, ltree, rtree, location);

    // Step 4: no match — error.
    if (op == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "operator does not exist for the given operand types");
        return nullptr;
    }

    // Step 5: reject shell operators.
    if (IsOperatorShell(op)) {
        ereport(pgcpp::error::LogLevel::kError, "operator is only a shell");
        return nullptr;
    }

    // Step 6: coerce arguments to the operator's declared operand types.
    //
    // Mirrors PostgreSQL's make_fn_arguments() (parse_func.c). When the
    // operator was selected via binary-coercibility (Stage 2 of
    // oper_select_candidate) rather than an exact match, the actual
    // argument types may differ from the operator's oprleft/oprright.
    // PostgreSQL inserts an implicit coercion node (RelabelType for
    // binary-compatible types, CoerceViaIO or FuncExpr otherwise) so the
    // executor receives arguments of the declared types.
    Oid left_actual = exprType(ltree);
    if (left_actual != op->oprleft) {
        Node* coerced = coerce_type(pstate, ltree, left_actual, op->oprleft, -1,
                                    CoercionContext::kImplicit, CoercionForm::kImplicit, location);
        if (coerced != nullptr)
            ltree = coerced;
    }
    Oid right_actual = exprType(rtree);
    if (right_actual != op->oprright) {
        Node* coerced = coerce_type(pstate, rtree, right_actual, op->oprright, -1,
                                    CoercionContext::kImplicit, CoercionForm::kImplicit, location);
        if (coerced != nullptr)
            rtree = coerced;
    }

    // Step 7: build the OpExpr.
    auto* opexpr = makeNode<OpExpr>();
    opexpr->opno = op->oid;
    opexpr->opfuncid = op->oprcode;
    opexpr->opresulttype = op->oprresult;
    opexpr->opretset = false;
    opexpr->opcollid = 0;
    opexpr->inputcollid = 0;
    opexpr->args.push_back(ltree);
    opexpr->args.push_back(rtree);
    opexpr->location = location;

    return opexpr;
}

// ---------------------------------------------------------------------------
// make_scalar_array_op — create a ScalarArrayOpExpr for IN/ANY/ALL.
//
// Mirrors PostgreSQL's make_scalar_array_op() in parse_oper.c.
// The operator is looked up as (left_type OP left_type) since the right
// operand is an array of the same element type. Unknown left operands
// are resolved to text.
// ---------------------------------------------------------------------------

Node* make_scalar_array_op(ParseState* pstate, const std::string& opname, bool useOr, Node* ltree,
                           Node* rtree, int location) {
    Oid left_type = exprType(ltree);

    // Look up the operator with both args as the left type.
    const FormData_pg_operator* op = binary_oper_exact(opname, left_type, left_type);

    if (op == nullptr) {
        // Candidate-based resolution.
        auto candidates = OpernameGetCandidates(opname);
        if (!candidates.empty()) {
            op = oper_select_candidate(candidates, left_type, left_type);
        }
    }

    if (op == nullptr && left_type == kUnknownOid) {
        // Unknown left operand — resolve to text.
        op = binary_oper_exact(opname, kTextOid, kTextOid);
        if (op != nullptr) {
            ltree = coerce_to_common_type(pstate, ltree, kTextOid, "IN");
        }
    }

    if (op == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "operator does not exist for IN/ANY expression");
        return nullptr;
    }

    if (IsOperatorShell(op)) {
        ereport(pgcpp::error::LogLevel::kError, "operator is only a shell");
        return nullptr;
    }

    auto* saop = makeNode<ScalarArrayOpExpr>();
    saop->opno = op->oid;
    saop->opfuncid = op->oprcode;
    saop->hashfuncid = 0;
    saop->negfuncid = 0;  // filled in by optimizer (PostgreSQL sets InvalidOid)
    saop->use_or = useOr;
    saop->inputcollid = 0;
    saop->args.push_back(ltree);
    saop->args.push_back(rtree);
    saop->location = location;

    return saop;
}

}  // namespace pgcpp::parser
