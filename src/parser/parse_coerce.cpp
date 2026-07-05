// parse_coerce.cpp — Type coercion functions for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_coerce.c.
// Implements type coercion between compatible types (e.g., int4 -> float8,
// unknown -> text). For ClickBench, we need numeric promotions and
// string type resolution.
#include "parser/parse_coerce.hpp"

#include <cstring>

#include "common/error/elog.hpp"
#include "parser/parse_type.hpp"
#include "types/builtins.hpp"
#include "types/datetime.hpp"
#include "types/datum.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::kInvalidOid;
using pgcpp::types::kBoolOid;
using pgcpp::types::kDateOid;
using pgcpp::types::kFloat4Oid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt2Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;
using pgcpp::types::kVarcharOid;

static constexpr Oid kUnknownOid = 705;

// ---------------------------------------------------------------------------
// Type category and preferred type helpers
// ---------------------------------------------------------------------------

enum class TypeCategory {
    kUnknown,
    kBoolean,
    kNumeric,
    kString,
    kDatetime,
    kOther,
};

static TypeCategory GetTypeCategory(Oid type_oid) {
    if (type_oid == kUnknownOid)
        return TypeCategory::kUnknown;
    if (type_oid == kBoolOid)
        return TypeCategory::kBoolean;
    if (type_oid == kInt2Oid || type_oid == kInt4Oid || type_oid == kInt8Oid ||
        type_oid == kFloat4Oid || type_oid == kFloat8Oid)
        return TypeCategory::kNumeric;
    if (type_oid == kTextOid || type_oid == kVarcharOid)
        return TypeCategory::kString;
    if (type_oid == kDateOid || type_oid == kTimestampOid)
        return TypeCategory::kDatetime;
    return TypeCategory::kOther;
}

// Get the preferred type in a category (for resolving unknowns).
[[maybe_unused]] static Oid GetPreferredType(TypeCategory cat) {
    switch (cat) {
        case TypeCategory::kBoolean:
            return kBoolOid;
        case TypeCategory::kNumeric:
            return kFloat8Oid;  // float8 is preferred
        case TypeCategory::kString:
            return kTextOid;  // text is preferred
        case TypeCategory::kDatetime:
            return kTimestampOid;
        default:
            return kInvalidOid;
    }
}

// ---------------------------------------------------------------------------
// IsBinaryCoercible — can source be binary-coerced to target?
// ---------------------------------------------------------------------------

bool IsBinaryCoercible(Oid srctype, Oid targettype) {
    if (srctype == targettype)
        return true;

    // unknown can be coerced to anything
    if (srctype == kUnknownOid)
        return true;

    // varchar <-> text are binary-compatible
    if ((srctype == kTextOid && targettype == kVarcharOid) ||
        (srctype == kVarcharOid && targettype == kTextOid))
        return true;

    // int2 -> int4 -> int8 are binary-compatible (widening)
    if (srctype == kInt2Oid && (targettype == kInt4Oid || targettype == kInt8Oid))
        return true;
    if (srctype == kInt4Oid && targettype == kInt8Oid)
        return true;

    // float4 -> float8 is binary-compatible
    if (srctype == kFloat4Oid && targettype == kFloat8Oid)
        return true;

    return false;
}

// ---------------------------------------------------------------------------
// can_coerce_type — can the input types be coerced to the target types?
// ---------------------------------------------------------------------------

bool can_coerce_type(int nargs, const Oid* input_typeids, const Oid* target_typeids,
                     CoercionContext ccontext) {
    for (int i = 0; i < nargs; ++i) {
        Oid input = input_typeids[i];
        Oid target = target_typeids[i];

        if (input == target)
            continue;

        // unknown can always be coerced
        if (input == kUnknownOid)
            continue;

        // Binary-coercible?
        if (IsBinaryCoercible(input, target))
            continue;

        // Assignment cast: allow widening numeric conversions
        if (ccontext == CoercionContext::kAssignment || ccontext == CoercionContext::kImplicit) {
            TypeCategory in_cat = GetTypeCategory(input);
            TypeCategory tgt_cat = GetTypeCategory(target);

            if (in_cat == TypeCategory::kNumeric && tgt_cat == TypeCategory::kNumeric) {
                // Allow int -> float conversions
                if ((input == kInt2Oid || input == kInt4Oid || input == kInt8Oid) &&
                    (target == kFloat4Oid || target == kFloat8Oid))
                    continue;
                // Allow float4 -> float8
                if (input == kFloat4Oid && target == kFloat8Oid)
                    continue;
            }

            // Allow date -> timestamp
            if (in_cat == TypeCategory::kDatetime && tgt_cat == TypeCategory::kDatetime) {
                if (input == kDateOid && target == kTimestampOid)
                    continue;
            }
        }

        // Explicit cast: allow more conversions
        if (ccontext == CoercionContext::kExplicit) {
            TypeCategory in_cat = GetTypeCategory(input);
            TypeCategory tgt_cat = GetTypeCategory(target);

            // Allow numeric -> string
            if (in_cat == TypeCategory::kNumeric && tgt_cat == TypeCategory::kString)
                continue;
            // Allow string -> numeric
            if (in_cat == TypeCategory::kString && tgt_cat == TypeCategory::kNumeric)
                continue;
            // Allow string -> datetime
            if (in_cat == TypeCategory::kString && tgt_cat == TypeCategory::kDatetime)
                continue;
            // Allow datetime -> string
            if (in_cat == TypeCategory::kDatetime && tgt_cat == TypeCategory::kString)
                continue;
        }

        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// coerce_type — coerce an expression to the target type.
// ---------------------------------------------------------------------------

Node* coerce_type([[maybe_unused]] ParseState* pstate, Node* node, Oid input_typeid,
                  Oid target_typeid, int target_typmod, CoercionContext ccontext,
                  CoercionForm cformat, int location) {
    if (input_typeid == target_typeid) {
        // No coercion needed, but may need a RelabelType for typmod
        return node;
    }

    // unknown -> target: if it's a Const, convert directly
    if (input_typeid == kUnknownOid) {
        if (node && node->GetTag() == pgcpp::nodes::NodeTag::kConst) {
            auto* con = static_cast<Const*>(node);
            // For string constants, we can directly set the target type
            // if the target is a string type
            if (target_typeid == kTextOid || target_typeid == kVarcharOid) {
                // Convert the C string Datum to a varlena Datum.
                // The unknown-type Const stores a char* to a null-terminated
                // string; TEXT/VARCHAR needs a varlena structure (4-byte
                // length header + data).
                const char* str = reinterpret_cast<const char*>(con->constvalue);
                if (str != nullptr) {
                    con->constvalue = pgcpp::types::text_in(str);
                }
                con->consttype = target_typeid;
                con->constlen = get_typlen(target_typeid);
                con->constbyval = get_typbyval(target_typeid);
                return node;
            }
            // For numeric targets, parse the string value
            if (target_typeid == kInt2Oid || target_typeid == kInt4Oid ||
                target_typeid == kInt8Oid || target_typeid == kFloat4Oid ||
                target_typeid == kFloat8Oid) {
                // The string is stored as a char* in the Datum
                const char* str = reinterpret_cast<const char*>(con->constvalue);
                if (str != nullptr) {
                    if (target_typeid == kInt2Oid) {
                        con->constvalue =
                            pgcpp::types::Int16GetDatum(static_cast<int16_t>(std::atoll(str)));
                    } else if (target_typeid == kInt4Oid) {
                        con->constvalue =
                            pgcpp::types::Int32GetDatum(static_cast<int32_t>(std::atoll(str)));
                    } else if (target_typeid == kInt8Oid) {
                        con->constvalue = pgcpp::types::Int64GetDatum(std::atoll(str));
                    } else if (target_typeid == kFloat4Oid) {
                        con->constvalue = pgcpp::types::Float4GetDatum(
                            static_cast<float>(std::strtod(str, nullptr)));
                    } else {
                        con->constvalue = pgcpp::types::Float8GetDatum(std::strtod(str, nullptr));
                    }
                    con->consttype = target_typeid;
                    con->constlen = get_typlen(target_typeid);
                    con->constbyval = get_typbyval(target_typeid);
                    return node;
                }
            }
            // For bool target
            if (target_typeid == kBoolOid) {
                const char* str = reinterpret_cast<const char*>(con->constvalue);
                if (str != nullptr) {
                    bool val = (strcmp(str, "t") == 0 || strcmp(str, "true") == 0 ||
                                strcmp(str, "1") == 0);
                    con->constvalue = pgcpp::types::BoolGetDatum(val);
                    con->consttype = kBoolOid;
                    con->constlen = 1;
                    con->constbyval = true;
                    return node;
                }
            }
            // For datetime targets (DATE, TIMESTAMP), parse via the type's
            // input function.
            if (target_typeid == kDateOid || target_typeid == kTimestampOid) {
                const char* str = reinterpret_cast<const char*>(con->constvalue);
                if (str != nullptr) {
                    if (target_typeid == kDateOid) {
                        con->constvalue = pgcpp::types::date_in(str);
                    } else {
                        con->constvalue = pgcpp::types::timestamp_in(str);
                    }
                    con->consttype = target_typeid;
                    con->constlen = get_typlen(target_typeid);
                    con->constbyval = get_typbyval(target_typeid);
                    return node;
                }
            }
        }
        // For non-Const unknown expressions, use CoerceViaIO
        auto* cio = makeNode<CoerceViaIO>();
        cio->arg = node;
        cio->resulttype = target_typeid;
        cio->resultcollid = 0;
        cio->coerceformat = cformat;
        cio->location = location;
        return cio;
    }

    // Binary-coercible: just relabel
    if (IsBinaryCoercible(input_typeid, target_typeid)) {
        auto* r = makeNode<RelabelType>();
        r->arg = node;
        r->resulttype = target_typeid;
        r->resulttypmod = target_typmod;
        r->resultcollid = 0;
        r->relabelformat = cformat;
        r->location = location;
        return r;
    }

    // Numeric widening (int -> float)
    TypeCategory in_cat = GetTypeCategory(input_typeid);
    TypeCategory tgt_cat = GetTypeCategory(target_typeid);

    if (in_cat == TypeCategory::kNumeric && tgt_cat == TypeCategory::kNumeric) {
        // Use RelabelType for binary-compatible, CoerceViaIO for others
        if (IsBinaryCoercible(input_typeid, target_typeid)) {
            auto* r = makeNode<RelabelType>();
            r->arg = node;
            r->resulttype = target_typeid;
            r->resulttypmod = target_typmod;
            r->resultcollid = 0;
            r->relabelformat = cformat;
            r->location = location;
            return r;
        }
        // For Const nodes, fold the narrowing/widening cast at parse time
        // so the executor doesn't need to evaluate CoerceViaIO. This mirrors
        // PostgreSQL's eval_const_expressions behavior for simple numeric casts.
        if (node != nullptr && node->GetTag() == pgcpp::nodes::NodeTag::kConst) {
            auto* con = static_cast<Const*>(node);
            if (con->constisnull) {
                // Just fix up the type metadata; value stays null.
                con->consttype = target_typeid;
                con->constlen = get_typlen(target_typeid);
                con->constbyval = get_typbyval(target_typeid);
                return node;
            }
            bool ok = false;
            if (input_typeid == kInt4Oid && target_typeid == kInt2Oid) {
                con->constvalue = pgcpp::types::Int16GetDatum(
                    static_cast<int16_t>(pgcpp::types::DatumGetInt32(con->constvalue)));
                ok = true;
            } else if (input_typeid == kInt4Oid && target_typeid == kInt8Oid) {
                // int4 -> int8 widening (e.g., LIMIT/OFFSET constants).
                con->constvalue = pgcpp::types::Int64GetDatum(
                    static_cast<int64_t>(pgcpp::types::DatumGetInt32(con->constvalue)));
                ok = true;
            } else if (input_typeid == kInt2Oid && target_typeid == kInt4Oid) {
                // int2 -> int4 widening.
                con->constvalue = pgcpp::types::Int32GetDatum(
                    static_cast<int32_t>(pgcpp::types::DatumGetInt16(con->constvalue)));
                ok = true;
            } else if (input_typeid == kInt2Oid && target_typeid == kInt8Oid) {
                // int2 -> int8 widening.
                con->constvalue = pgcpp::types::Int64GetDatum(
                    static_cast<int64_t>(pgcpp::types::DatumGetInt16(con->constvalue)));
                ok = true;
            } else if (input_typeid == kInt8Oid && target_typeid == kInt4Oid) {
                con->constvalue = pgcpp::types::Int32GetDatum(
                    static_cast<int32_t>(pgcpp::types::DatumGetInt64(con->constvalue)));
                ok = true;
            } else if (input_typeid == kInt8Oid && target_typeid == kInt2Oid) {
                con->constvalue = pgcpp::types::Int16GetDatum(
                    static_cast<int16_t>(pgcpp::types::DatumGetInt64(con->constvalue)));
                ok = true;
            } else if (input_typeid == kFloat8Oid && target_typeid == kFloat4Oid) {
                con->constvalue = pgcpp::types::Float4GetDatum(
                    static_cast<float>(pgcpp::types::DatumGetFloat8(con->constvalue)));
                ok = true;
            } else if ((input_typeid == kInt2Oid || input_typeid == kInt4Oid ||
                        input_typeid == kInt8Oid) &&
                       (target_typeid == kFloat4Oid || target_typeid == kFloat8Oid)) {
                // int -> float: extract integer value, convert to double/float.
                int64_t ival = 0;
                if (input_typeid == kInt2Oid) {
                    ival = pgcpp::types::DatumGetInt16(con->constvalue);
                } else if (input_typeid == kInt4Oid) {
                    ival = pgcpp::types::DatumGetInt32(con->constvalue);
                } else {
                    ival = pgcpp::types::DatumGetInt64(con->constvalue);
                }
                if (target_typeid == kFloat4Oid) {
                    con->constvalue = pgcpp::types::Float4GetDatum(static_cast<float>(ival));
                } else {
                    con->constvalue = pgcpp::types::Float8GetDatum(static_cast<double>(ival));
                }
                ok = true;
            } else if (input_typeid == kFloat4Oid && target_typeid == kFloat8Oid) {
                // float4 -> float8 widening (not binary-coercible in our model).
                con->constvalue = pgcpp::types::Float8GetDatum(
                    static_cast<double>(pgcpp::types::DatumGetFloat4(con->constvalue)));
                ok = true;
            } else if (input_typeid == kFloat8Oid && target_typeid == kInt2Oid) {
                con->constvalue = pgcpp::types::Int16GetDatum(
                    static_cast<int16_t>(pgcpp::types::DatumGetFloat8(con->constvalue)));
                ok = true;
            } else if (input_typeid == kFloat8Oid && target_typeid == kInt4Oid) {
                con->constvalue = pgcpp::types::Int32GetDatum(
                    static_cast<int32_t>(pgcpp::types::DatumGetFloat8(con->constvalue)));
                ok = true;
            } else if (input_typeid == kFloat8Oid && target_typeid == kInt8Oid) {
                con->constvalue = pgcpp::types::Int64GetDatum(
                    static_cast<int64_t>(pgcpp::types::DatumGetFloat8(con->constvalue)));
                ok = true;
            } else if (input_typeid == kFloat4Oid && target_typeid == kInt2Oid) {
                con->constvalue = pgcpp::types::Int16GetDatum(
                    static_cast<int16_t>(pgcpp::types::DatumGetFloat4(con->constvalue)));
                ok = true;
            } else if (input_typeid == kFloat4Oid && target_typeid == kInt4Oid) {
                con->constvalue = pgcpp::types::Int32GetDatum(
                    static_cast<int32_t>(pgcpp::types::DatumGetFloat4(con->constvalue)));
                ok = true;
            } else if (input_typeid == kFloat4Oid && target_typeid == kInt8Oid) {
                con->constvalue = pgcpp::types::Int64GetDatum(
                    static_cast<int64_t>(pgcpp::types::DatumGetFloat4(con->constvalue)));
                ok = true;
            }
            if (ok) {
                con->consttype = target_typeid;
                con->constlen = get_typlen(target_typeid);
                con->constbyval = get_typbyval(target_typeid);
                return node;
            }
        }
        // Non-Const or unsupported numeric cast: fall back to CoerceViaIO.
        auto* cio = makeNode<CoerceViaIO>();
        cio->arg = node;
        cio->resulttype = target_typeid;
        cio->resultcollid = 0;
        cio->coerceformat = cformat;
        cio->location = location;
        return cio;
    }

    // String <-> numeric/datetime via I/O
    if ((in_cat == TypeCategory::kString && tgt_cat != TypeCategory::kString) ||
        (in_cat != TypeCategory::kString && tgt_cat == TypeCategory::kString)) {
        auto* cio = makeNode<CoerceViaIO>();
        cio->arg = node;
        cio->resulttype = target_typeid;
        cio->resultcollid = 0;
        cio->coerceformat = cformat;
        cio->location = location;
        return cio;
    }

    // If we get here, the coercion is not supported
    if (ccontext >= CoercionContext::kAssignment) {
        ereport(pgcpp::error::LogLevel::kError,
                "cannot coerce type " + std::to_string(input_typeid) + " to target type " +
                    std::to_string(target_typeid) + " in coerce_type");
    }

    return node;  // return as-is if can't coerce
}

// ---------------------------------------------------------------------------
// coerce_to_target_type — coerce an expression to a target type/modifier.
// ---------------------------------------------------------------------------

Node* coerce_to_target_type(ParseState* pstate, Node* expr, Oid expr_type, Oid target_type,
                            int target_typmod, CoercionContext ccontext, CoercionForm cformat,
                            int location) {
    return coerce_type(pstate, expr, expr_type, target_type, target_typmod, ccontext, cformat,
                       location);
}

// ---------------------------------------------------------------------------
// select_common_type — select a common type for a list of expressions.
// ---------------------------------------------------------------------------

Oid select_common_type([[maybe_unused]] ParseState* pstate, const std::vector<Node*>& exprs,
                       [[maybe_unused]] const char* context, Node** which_expr) {
    if (exprs.empty())
        return kInvalidOid;

    Oid common_type = kUnknownOid;
    bool have_unknown = false;
    bool have_numeric = false;
    bool have_string = false;

    for (size_t i = 0; i < exprs.size(); ++i) {
        Node* expr = exprs[i];
        Oid expr_type = exprType(expr);

        if (expr_type == kUnknownOid) {
            have_unknown = true;
            continue;
        }

        if (common_type == kUnknownOid) {
            common_type = expr_type;
            if (which_expr)
                *which_expr = expr;
        } else {
            // Find a common type
            TypeCategory a = GetTypeCategory(common_type);
            TypeCategory b = GetTypeCategory(expr_type);

            if (a == TypeCategory::kNumeric && b == TypeCategory::kNumeric) {
                // Promote to the wider type
                if (common_type == kInt2Oid) {
                    common_type = (expr_type == kInt8Oid) ? kInt8Oid : kInt4Oid;
                } else if (common_type == kInt4Oid && expr_type == kInt8Oid) {
                    common_type = kInt8Oid;
                }
                if (expr_type == kFloat8Oid || common_type == kFloat8Oid) {
                    common_type = kFloat8Oid;
                } else if (expr_type == kFloat4Oid || common_type == kFloat4Oid) {
                    common_type = (common_type == kInt8Oid) ? kFloat8Oid : kFloat4Oid;
                }
            } else if (a == TypeCategory::kString && b == TypeCategory::kString) {
                // text is preferred
                common_type = kTextOid;
            } else if (a != b) {
                // Different categories: prefer the preferred type
                if (a == TypeCategory::kUnknown) {
                    common_type = expr_type;
                }
                // If categories differ and neither is unknown, use text
                if (common_type != kUnknownOid && a != b) {
                    common_type = kTextOid;
                }
            }
        }

        if (type_is_numeric(expr_type))
            have_numeric = true;
        if (type_is_string(expr_type))
            have_string = true;
    }

    // If all expressions were unknown, resolve to text
    if (common_type == kUnknownOid) {
        common_type = kTextOid;
    }

    (void)have_unknown;
    (void)have_numeric;
    (void)have_string;

    return common_type;
}

// ---------------------------------------------------------------------------
// coerce_to_common_type — coerce an expression to a common type.
// ---------------------------------------------------------------------------

Node* coerce_to_common_type(ParseState* pstate, Node* node, Oid common_type,
                            [[maybe_unused]] const char* context) {
    Oid node_type = exprType(node);
    if (node_type == common_type)
        return node;

    Node* result = coerce_type(pstate, node, node_type, common_type, -1, CoercionContext::kImplicit,
                               CoercionForm::kImplicit, -1);
    if (result == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "cannot coerce to common type");
    }
    return result;
}

// ---------------------------------------------------------------------------
// exprType — get the type OID of an expression node.
// This is a utility function used throughout parse analysis.
Oid exprType(const Node* expr) {
    if (expr == nullptr)
        return kUnknownOid;

    using pgcpp::nodes::NodeTag;
    switch (expr->GetTag()) {
        case NodeTag::kVar: {
            const auto* v = static_cast<const Var*>(expr);
            return v->vartype;
        }
        case NodeTag::kConst: {
            const auto* c = static_cast<const Const*>(expr);
            return c->consttype;
        }
        case NodeTag::kParam: {
            const auto* p = static_cast<const Param*>(expr);
            return p->paramtype;
        }
        case NodeTag::kOpExpr: {
            const auto* op = static_cast<const OpExpr*>(expr);
            return op->opresulttype;
        }
        case NodeTag::kFuncExpr: {
            const auto* f = static_cast<const FuncExpr*>(expr);
            return f->funcresulttype;
        }
        case NodeTag::kAggref: {
            const auto* a = static_cast<const Aggref*>(expr);
            return a->aggtype;
        }
        case NodeTag::kBoolExpr:
            return kBoolOid;
        case NodeTag::kNullTest:
            return kBoolOid;
        case NodeTag::kBooleanTest:
            return kBoolOid;
        case NodeTag::kCaseExpr: {
            const auto* c = static_cast<const CaseExpr*>(expr);
            return c->casetype;
        }
        case NodeTag::kRelabelType: {
            const auto* r = static_cast<const RelabelType*>(expr);
            return r->resulttype;
        }
        case NodeTag::kCoerceViaIO: {
            const auto* c = static_cast<const CoerceViaIO*>(expr);
            return c->resulttype;
        }
        case NodeTag::kCoerceToDomain: {
            const auto* c = static_cast<const CoerceToDomain*>(expr);
            return c->resulttype;
        }
        case NodeTag::kScalarArrayOpExpr:
            return kBoolOid;
        case NodeTag::kSubLink: {
            const auto* s = static_cast<const SubLink*>(expr);
            if (s->subselect) {
                return exprType(s->subselect);
            }
            return kUnknownOid;
        }
        case NodeTag::kTargetEntry: {
            const auto* t = static_cast<const TargetEntry*>(expr);
            return exprType(t->expr);
        }
        default:
            return kUnknownOid;
    }
}

// exprTypmod — get the type modifier of an expression node.
int exprTypmod(const Node* expr) {
    if (expr == nullptr)
        return -1;

    using pgcpp::nodes::NodeTag;
    switch (expr->GetTag()) {
        case NodeTag::kVar:
            return static_cast<const Var*>(expr)->vartypmod;
        case NodeTag::kConst:
            return static_cast<const Const*>(expr)->consttypmod;
        case NodeTag::kParam:
            return static_cast<const Param*>(expr)->paramtypmod;
        case NodeTag::kRelabelType:
            return static_cast<const RelabelType*>(expr)->resulttypmod;
        case NodeTag::kFuncExpr:
            return -1;  // simplified
        case NodeTag::kOpExpr:
            return -1;
        case NodeTag::kCaseExpr:
            return -1;
        case NodeTag::kTargetEntry:
            return exprTypmod(static_cast<const TargetEntry*>(expr)->expr);
        default:
            return -1;
    }
}

}  // namespace pgcpp::parser
