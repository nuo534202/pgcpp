// parse_func.cpp — Function lookup functions for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_func.c.
//
// Implements catalog-driven function lookup and FuncExpr/Aggref construction,
// preserving PostgreSQL's resolution algorithm:
//   1. Find candidate functions by name (from pg_proc via Catalog).
//   2. Filter candidates by argument count.
//   3. Select the best candidate using exact-match counting and
//      binary-coercibility (func_match_argtypes / func_select_candidate).
//   4. Coerce arguments to the function's declared types (make_fn_arguments).
//   5. Build FuncExpr (regular functions) or Aggref (aggregates).
//
// All function metadata is read from the Catalog (pg_proc rows populated
// by BootstrapCatalog). No hardcoded function table remains.

#include "pgcpp/parser/parse_func.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_proc.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/parse_coerce.hpp"
#include "pgcpp/parser/parse_expr.hpp"
#include "pgcpp/parser/parse_type.hpp"
#include "pgcpp/types/datum.hpp"

namespace mytoydb::parser {

using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_proc;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::ProKind;
using mytoydb::nodes::Value;
using mytoydb::types::kInt8Oid;

// UNKNOWNOID — PostgreSQL's OID for the "unknown" pseudo-type (705).
static constexpr Oid kUnknownOid = 705;

namespace {

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// FuncnameGetCandidates — return all pg_proc rows with the given name
// and the specified argument count.
//
// Mirrors PostgreSQL's FuncnameGetCandidates (namespace search omitted;
// MyToyDB has a single namespace for built-ins). Returns candidates in
// catalog insertion order, which matters for tie-breaking.
std::vector<const FormData_pg_proc*> FuncnameGetCandidates(const std::string& funcname, int nargs) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return {};
    auto all = cat->GetProcsByName(funcname);
    std::vector<const FormData_pg_proc*> result;
    for (const auto* proc : all) {
        if (static_cast<int>(proc->proargtypes.size()) == nargs) {
            result.push_back(proc);
        }
    }
    return result;
}

// func_match_argtypes — classify a candidate by how well its arg types match.
//
// Mirrors PostgreSQL's func_match_argtypes (parse_func.c).
// A candidate is viable if every input type is either an exact match or
// binary-coercible to the declared type. Sets *exact_count to the number
// of exact matches and *coerce_count to the number of coercion matches.
// Returns true if the candidate is viable.
bool func_match_argtypes(const FormData_pg_proc* candidate, const std::vector<Oid>& input_types,
                         int* exact_count, int* coerce_count) {
    *exact_count = 0;
    *coerce_count = 0;
    const auto& decl_types = candidate->proargtypes;
    if (decl_types.size() != input_types.size())
        return false;
    for (size_t i = 0; i < input_types.size(); ++i) {
        if (input_types[i] == decl_types[i]) {
            ++(*exact_count);
        } else if (IsBinaryCoercible(input_types[i], decl_types[i])) {
            ++(*coerce_count);
        } else {
            return false;  // not viable
        }
    }
    return true;
}

// func_select_candidate — select the best function candidate.
//
// Mirrors PostgreSQL's func_select_candidate (parse_func.c).
// Algorithm:
//   1. Filter to viable candidates (all args exact or binary-coercible).
//   2. Select candidates with the most exact matches.
//   3. If there's a tie, return the first one. (PostgreSQL errors on
//      ambiguity; MyToyDB's limited type set makes this rare and the
//      fallback avoids false errors.)
const FormData_pg_proc* func_select_candidate(
    const std::vector<const FormData_pg_proc*>& candidates, const std::vector<Oid>& input_types) {
    if (candidates.empty())
        return nullptr;

    // Step 1: find viable candidates and track the best exact-match count.
    std::vector<const FormData_pg_proc*> viable;
    int best_exact = -1;
    for (const auto* cand : candidates) {
        int exact = 0, coerce = 0;
        if (func_match_argtypes(cand, input_types, &exact, &coerce)) {
            viable.push_back(cand);
            if (exact > best_exact)
                best_exact = exact;
        }
    }
    if (viable.empty())
        return nullptr;
    if (viable.size() == 1)
        return viable[0];

    // Step 2: keep only candidates with the most exact matches.
    std::vector<const FormData_pg_proc*> best;
    for (const auto* cand : viable) {
        int exact = 0, coerce = 0;
        func_match_argtypes(cand, input_types, &exact, &coerce);
        if (exact == best_exact)
            best.push_back(cand);
    }
    if (best.size() == 1)
        return best[0];

    // Step 3: return the first candidate (PostgreSQL errors on ambiguity).
    return best[0];
}

// make_fn_arguments — coerce arguments to the function's declared types.
//
// Mirrors PostgreSQL's make_fn_arguments (parse_func.c). After candidate
// selection, the actual argument types may differ from the declared types
// (when matched via binary-coercibility). This inserts implicit coercion
// nodes (RelabelType for binary-compatible, CoerceViaIO otherwise) so the
// executor receives arguments of the declared types.
void make_fn_arguments(ParseState* pstate, std::vector<Node*>& args, const FormData_pg_proc* proc,
                       int location) {
    const auto& decl_types = proc->proargtypes;
    for (size_t i = 0; i < args.size() && i < decl_types.size(); ++i) {
        Oid actual_type = exprType(args[i]);
        if (actual_type != decl_types[i]) {
            Node* coerced =
                coerce_type(pstate, args[i], actual_type, decl_types[i], -1,
                            CoercionContext::kImplicit, CoercionForm::kImplicit, location);
            if (coerced != nullptr)
                args[i] = coerced;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// IsAggregateFunction — check if a function name refers to an aggregate.
//
// Mirrors PostgreSQL's aggregate lookup: scans pg_proc for any entry with
// the given name and prokind = kAggregate.
// ---------------------------------------------------------------------------

bool IsAggregateFunction(const std::string& funcname) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return false;
    auto procs = cat->GetProcsByName(to_lower(funcname));
    for (const auto* proc : procs) {
        if (proc->prokind == ProKind::kAggregate)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// LookupFuncName — look up a function by name and argument types.
//
// Mirrors PostgreSQL's LookupFuncName / LookupFuncNameArgs. Returns true
// and fills *result if a matching function is found, false otherwise.
// ---------------------------------------------------------------------------

bool LookupFuncName(const std::vector<std::string>& funcname, int nargs, const Oid* argtypes,
                    FuncLookupResult* result) {
    if (funcname.empty())
        return false;
    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return false;

    std::string name = to_lower(funcname.back());
    auto candidates = FuncnameGetCandidates(name, nargs);
    if (candidates.empty())
        return false;

    std::vector<Oid> input_types(argtypes, argtypes + nargs);
    const FormData_pg_proc* proc = func_select_candidate(candidates, input_types);
    if (proc == nullptr)
        return false;

    result->funcid = proc->oid;
    result->rettype = proc->prorettype;
    result->retset = proc->proretset;
    result->is_aggregate = (proc->prokind == ProKind::kAggregate);
    return true;
}

// ---------------------------------------------------------------------------
// transformFuncCall — transform a FuncCall (raw parse tree) into a
// FuncExpr or Aggref (transformed expression tree).
//
// Mirrors PostgreSQL's ParseFuncOrColumn (parse_func.c).
// Algorithm:
//   1. Build the function name from the funcname list.
//   2. Transform all arguments.
//   3. Handle COUNT(*) specially (no catalog lookup needed).
//   4. Find candidate functions by name and arg count from the catalog.
//   5. Select the best candidate (exact match → binary-coercibility).
//   6. Coerce arguments to declared types (make_fn_arguments).
//   7. Build Aggref (aggregate) or FuncExpr (regular function).
// ---------------------------------------------------------------------------

Node* transformFuncCall(ParseState* pstate, FuncCall* fn, int location) {
    // All C++ locals with non-trivial destructors (std::string, std::vector)
    // are confined to the block below. ereport(ERROR) does longjmp, which
    // bypasses C++ destructors; by setting `errmsg` and ereport'ing AFTER the
    // block, we ensure locals are properly destructed and don't leak.
    Node* result = nullptr;
    const char* errmsg = nullptr;

    {
        // Step 1: build the function name from the funcname list.
        // MyToyDB has a single namespace, so only the last component (the
        // function name itself) is used for catalog lookup; any schema prefix
        // (e.g. "pg_catalog") is ignored.
        std::string funcname;
        for (size_t i = 0; i < fn->funcname.size(); ++i) {
            auto* v = static_cast<Value*>(fn->funcname[i]);
            if (i > 0)
                funcname += ".";
            funcname += v->GetString();
        }
        funcname = to_lower(funcname);
        std::string simple_name = funcname;
        size_t dot_pos = simple_name.rfind('.');
        if (dot_pos != std::string::npos)
            simple_name = simple_name.substr(dot_pos + 1);

        // Step 2: transform the arguments.
        std::vector<Node*> targs;
        for (Node* arg : fn->args) {
            targs.push_back(transformExpr(pstate, arg, pstate->p_expr_kind));
        }

        int nargs = static_cast<int>(targs.size());

        // Step 3: handle COUNT(*) specially.
        //
        // PostgreSQL's count() is declared with proargtypes = {anyelement}, so
        // COUNT(*) (no actual argument) is resolved to the 0-arg form. Since
        // MyToyDB does not model polymorphic types, we handle COUNT(*) here
        // without a catalog lookup.
        if (simple_name == "count" && fn->agg_star) {
            auto* agg = makeNode<Aggref>();
            agg->aggfnoid = 2147;  // PostgreSQL's count(anyelement) OID
            agg->aggtype = kInt8Oid;
            agg->aggstar = true;
            agg->aggkind = 'n';
            agg->agglevelsup = 0;
            agg->aggsplit = AggSplit::kSimple;
            agg->location = location;

            // Handle FILTER clause (same as the general aggregate path below).
            if (fn->agg_filter != nullptr) {
                agg->aggfilter = transformExpr(pstate, fn->agg_filter, ParseExprKind::kFilter);
            }

            pstate->p_has_aggs = true;
            result = agg;
        } else {
            // Step 4: find candidate functions by name and arg count.
            auto candidates = FuncnameGetCandidates(simple_name, nargs);
            if (candidates.empty()) {
                errmsg = "function does not exist";
            } else {
                // Step 5: select the best candidate.
                std::vector<Oid> input_types;
                input_types.reserve(targs.size());
                for (Node* arg : targs) {
                    input_types.push_back(exprType(arg));
                }
                const FormData_pg_proc* proc = func_select_candidate(candidates, input_types);
                if (proc == nullptr) {
                    errmsg = "function does not exist for the given argument types";
                } else {
                    // Step 6: coerce arguments to the function's declared types.
                    make_fn_arguments(pstate, targs, proc, location);

                    // Step 7: build Aggref (aggregate) or FuncExpr (regular function).
                    if (proc->prokind == ProKind::kAggregate) {
                        auto* agg = makeNode<Aggref>();
                        agg->aggfnoid = proc->oid;
                        agg->aggtype = proc->prorettype;
                        agg->aggstar = fn->agg_star;
                        agg->aggkind = 'n';
                        agg->agglevelsup = 0;
                        agg->aggsplit = AggSplit::kSimple;
                        agg->location = location;

                        if (!targs.empty()) {
                            agg->args = targs;
                        }

                        // Handle FILTER clause.
                        if (fn->agg_filter != nullptr) {
                            agg->aggfilter =
                                transformExpr(pstate, fn->agg_filter, ParseExprKind::kFilter);
                        }

                        // Handle ORDER BY within aggregate.
                        if (!fn->agg_order.empty()) {
                            for (Node* sortnode : fn->agg_order) {
                                Node* transformed =
                                    transformExpr(pstate, sortnode, ParseExprKind::kOrderBy);
                                agg->aggorder.push_back(transformed);
                            }
                        }

                        pstate->p_has_aggs = true;
                        result = agg;
                    } else {
                        // Regular function.
                        auto* funcexpr = makeNode<FuncExpr>();
                        funcexpr->funcid = proc->oid;
                        funcexpr->funcresulttype = proc->prorettype;
                        funcexpr->funcretset = proc->proretset;
                        funcexpr->funcvariadic = false;
                        funcexpr->funcformat = CoercionForm::kExplicit;
                        funcexpr->funccollid = 0;
                        funcexpr->inputcollid = 0;
                        funcexpr->args = targs;
                        funcexpr->location = location;

                        result = funcexpr;
                    }
                }
            }
        }
    }
    // All locals (funcname, simple_name, targs, candidates, input_types) are
    // now destructed — safe to ereport.

    if (errmsg != nullptr) {
        ereport(mytoydb::error::LogLevel::kError, errmsg);
    }
    return result;
}

}  // namespace mytoydb::parser
