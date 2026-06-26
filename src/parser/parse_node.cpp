// parse_node.cpp — ParseState management and helper functions.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_node.c.
// Provides make_parsestate, free_parsestate, make_const, and boolean
// expression constructors.

#include "mytoydb/parser/parse_node.hpp"

#include <cerrno>
#include <cstdlib>
#include <new>
#include <string>

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::parser {
using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::makePallocNode;

using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;
using mytoydb::nodes::Value;
using mytoydb::types::BoolGetDatum;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::Float8GetDatum;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::Int64GetDatum;

// UNKNOWNOID — PostgreSQL's OID for the "unknown" type (705).
// Used for string literals before type coercion resolves them.
static constexpr Oid kUnknownOid = 705;

// ---------------------------------------------------------------------------
// ParseState management
// ---------------------------------------------------------------------------

ParseState* make_parsestate(ParseState* parent) {
    auto* pstate = makePallocNode<ParseState>();
    pstate->parent_parse_state = parent;
    pstate->p_next_resno = 1;
    pstate->p_resolve_unknowns = true;
    if (parent) {
        pstate->p_sourcetext = parent->p_sourcetext;
    }
    return pstate;
}

void free_parsestate(ParseState* pstate) {
    destroyPallocNode(pstate);
}

// ---------------------------------------------------------------------------
// make_const — convert an A_Const (raw parse tree) to a Const (transformed).
// ---------------------------------------------------------------------------

Const* make_const(ParseState* pstate, AConst* aconst) {
    if (aconst->isnull) {
        // Return a null const of unknown type.
        auto* con = makeConst(kUnknownOid, -1, 0, -2, 0, true, false, aconst->location);
        return con;
    }

    // Boolean constants (TRUE/FALSE keywords) produce bool Oid directly.
    if (aconst->isbool) {
        auto* v = static_cast<Value*>(aconst->val);
        bool bval = (v->GetInteger() != 0);
        auto* con = makeConst(mytoydb::types::kBoolOid, -1, 0, 1,
                              mytoydb::types::BoolGetDatum(bval), false, true, aconst->location);
        return con;
    }

    Node* val = aconst->val;
    if (val == nullptr) {
        auto* con = makeConst(kUnknownOid, -1, 0, -2, 0, true, false, aconst->location);
        return con;
    }

    NodeTag tag = nodeTag(val);
    Datum datum = 0;
    Oid typeid_ = 0;
    int typelen = 0;
    bool typebyval = false;

    switch (tag) {
        case NodeTag::kInteger: {
            auto* v = static_cast<Value*>(val);
            int64_t ival = v->GetInteger();
            // Check if it fits in int32
            int32_t val32 = static_cast<int32_t>(ival);
            if (ival == static_cast<int64_t>(val32)) {
                datum = Int32GetDatum(val32);
                typeid_ = mytoydb::types::kInt4Oid;
                typelen = sizeof(int32_t);
                typebyval = true;
            } else {
                datum = Int64GetDatum(ival);
                typeid_ = mytoydb::types::kInt8Oid;
                typelen = sizeof(int64_t);
                typebyval = true;
            }
            break;
        }
        case NodeTag::kFloat: {
            auto* v = static_cast<Value*>(val);
            const std::string& fval = v->GetFloat();
            // Try to parse as integer first (could be an oversize integer)
            errno = 0;
            char* endptr = nullptr;
            long long val64 = std::strtoll(fval.c_str(), &endptr, 10);
            if (errno == 0 && endptr != nullptr && *endptr == '\0') {
                // It's an integer
                int32_t val32 = static_cast<int32_t>(val64);
                if (val64 == static_cast<long long>(val32)) {
                    datum = Int32GetDatum(val32);
                    typeid_ = mytoydb::types::kInt4Oid;
                    typelen = sizeof(int32_t);
                    typebyval = true;
                } else {
                    datum = Int64GetDatum(static_cast<int64_t>(val64));
                    typeid_ = mytoydb::types::kInt8Oid;
                    typelen = sizeof(int64_t);
                    typebyval = true;
                }
            } else {
                // It's a float
                double dval = std::strtod(fval.c_str(), &endptr);
                datum = Float8GetDatum(dval);
                typeid_ = mytoydb::types::kFloat8Oid;
                typelen = sizeof(double);
                typebyval = true;
            }
            break;
        }
        case NodeTag::kString: {
            auto* v = static_cast<Value*>(val);
            // String literals start as unknown type; will be coerced later.
            // We store the C string pointer as the Datum (by reference).
            // For now, we use the unknown type and store the string pointer.
            // The executor will handle the actual string storage.
            typeid_ = kUnknownOid;
            typelen = -2;  // cstring-style varwidth
            typebyval = false;
            // Store the string in a palloc'd buffer so it survives
            const std::string& sval = v->GetString();
            char* buf = static_cast<char*>(mytoydb::memory::palloc(sval.size() + 1));
            std::memcpy(buf, sval.data(), sval.size());
            buf[sval.size()] = '\0';
            datum = reinterpret_cast<Datum>(buf);
            break;
        }
        default:
            ereport(mytoydb::error::LogLevel::kError, "unrecognized constant type in make_const");
            return nullptr;  // keep compiler quiet
    }

    auto* con = makeConst(typeid_, -1, 0, typelen, datum, false, typebyval, aconst->location);
    return con;
}

// ---------------------------------------------------------------------------
// Boolean expression constructors
// ---------------------------------------------------------------------------

Node* makeBoolConst(bool value, bool isnull) {
    auto* con = makeNode<Const>();
    con->consttype = mytoydb::types::kBoolOid;
    con->consttypmod = -1;
    con->constcollid = 0;
    con->constlen = 1;
    con->constisnull = isnull;
    con->constbyval = true;
    con->constvalue = isnull ? 0 : BoolGetDatum(value);
    con->location = -1;
    return con;
}

Node* make_andclause(std::vector<Node*> args) {
    auto* expr = makeNode<BoolExpr>();
    expr->boolop = BoolExprType::kAnd;
    expr->args = std::move(args);
    expr->location = -1;
    return expr;
}

Node* make_orclause(std::vector<Node*> args) {
    auto* expr = makeNode<BoolExpr>();
    expr->boolop = BoolExprType::kOr;
    expr->args = std::move(args);
    expr->location = -1;
    return expr;
}

Node* make_notclause(Node* arg) {
    auto* expr = makeNode<BoolExpr>();
    expr->boolop = BoolExprType::kNot;
    expr->args.push_back(arg);
    expr->location = -1;
    return expr;
}

Node* make_ands_implicit(std::vector<Node*> andclauses) {
    if (andclauses.empty())
        return nullptr;
    if (andclauses.size() == 1)
        return andclauses[0];
    return make_andclause(std::move(andclauses));
}

}  // namespace mytoydb::parser
