// outfuncs.cpp — node tree serialization (Node → string).
//
// Converted from PostgreSQL 15's src/backend/nodes/outfuncs.c.
//
// Implements nodeToString() and per-node-type _out functions for core
// node types. The serialization format is PG's Lisp-like S-expression:
//
//   (TAG :fieldname value :fieldname value ...)
//
// See outfuncs.hpp for format details.
#include "common/containers/outfuncs.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "common/containers/node.hpp"
#include "common/containers/string_info.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

namespace pgcpp::nodes {

using pgcpp::containers::StringInfo;
using pgcpp::parser::BooleanTest;
using pgcpp::parser::BoolExpr;
using pgcpp::parser::Const;
using pgcpp::parser::FuncExpr;
using pgcpp::parser::NullTest;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Param;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;

// --- Forward declarations of per-type _out functions ---

static void outNode(StringInfo* buf, const Node* node);
static void outValue(StringInfo* buf, const Value* value);
static void outVar(StringInfo* buf, const Var* node);
static void outConst(StringInfo* buf, const Const* node);
static void outParam(StringInfo* buf, const Param* node);
static void outOpExpr(StringInfo* buf, const OpExpr* node);
static void outFuncExpr(StringInfo* buf, const FuncExpr* node);
static void outBoolExpr(StringInfo* buf, const BoolExpr* node);
static void outNullTest(StringInfo* buf, const NullTest* node);
static void outBooleanTest(StringInfo* buf, const BooleanTest* node);
static void outTargetEntry(StringInfo* buf, const TargetEntry* node);
static void outRangeTblEntry(StringInfo* buf, const RangeTblEntry* node);
static void outQuery(StringInfo* buf, const Query* node);

// --- Helper functions ---

void outInt(StringInfo* buf, const char* label, int value) {
    buf->AppendPrintf(" :%s %d", label, value);
}

void outInt64(StringInfo* buf, const char* label, int64_t value) {
    buf->AppendPrintf(" :%s %lld", label, static_cast<long long>(value));
}

void outBool(StringInfo* buf, const char* label, bool value) {
    buf->AppendPrintf(" :%s %s", label, value ? "t" : "f");
}

void outOid(StringInfo* buf, const char* label, unsigned int value) {
    buf->AppendPrintf(" :%s %u", label, value);
}

void outString(StringInfo* buf, const char* label, std::string_view value) {
    if (value.empty()) {
        buf->AppendPrintf(" :%s <>", label);
    } else {
        buf->AppendPrintf(" :%s %.*s", label, static_cast<int>(value.size()), value.data());
    }
}

void outChar(StringInfo* buf, const char* label, char value) {
    if (value == 0) {
        buf->AppendPrintf(" :%s 0", label);
    } else {
        buf->AppendPrintf(" :%s %c", label, value);
    }
}

void outNodeField(StringInfo* buf, const char* label, const Node* value) {
    if (value == nullptr) {
        buf->AppendPrintf(" :%s <>", label);
    } else {
        buf->AppendPrintf(" :%s ", label);
        outNode(buf, value);
    }
}

void outNodeList(StringInfo* buf, const char* label, const std::vector<Node*>& list) {
    if (list.empty()) {
        buf->AppendPrintf(" :%s ()", label);
    } else {
        buf->AppendPrintf(" :%s (", label);
        for (auto* item : list) {
            outNode(buf, item);
            buf->AppendChar(' ');
        }
        // Remove trailing space and close paren
        if (buf->Length() > 0 && buf->Data()[buf->Length() - 1] == ' ') {
            buf->Str().pop_back();
        }
        buf->AppendChar(')');
    }
}

// --- Tag name mapping ---

static const char* tagName(NodeTag tag) {
    switch (tag) {
        case NodeTag::kVar:
            return "VAR";
        case NodeTag::kConst:
            return "CONST";
        case NodeTag::kParam:
            return "PARAM";
        case NodeTag::kOpExpr:
            return "OPEXPR";
        case NodeTag::kFuncExpr:
            return "FUNCEXPR";
        case NodeTag::kBoolExpr:
            return "BOOLEXPR";
        case NodeTag::kNullTest:
            return "NULLTEST";
        case NodeTag::kBooleanTest:
            return "BOOLEANTEST";
        case NodeTag::kTargetEntry:
            return "TARGETENTRY";
        case NodeTag::kRangeTblEntry:
            return "RTE";
        case NodeTag::kQuery:
            return "QUERY";
        case NodeTag::kInteger:
            return "INTEGER";
        case NodeTag::kFloat:
            return "FLOAT";
        case NodeTag::kString:
            return "STRING";
        case NodeTag::kNull:
            return "NULL";
        default:
            return "UNKNOWN";
    }
}

// --- Dispatch ---

static void outNode(StringInfo* buf, const Node* node) {
    if (node == nullptr) {
        buf->AppendString("<>");
        return;
    }

    NodeTag tag = node->GetTag();

    // Open paren + tag name
    buf->AppendChar('(');
    buf->AppendString(tagName(tag));

    switch (tag) {
        case NodeTag::kInteger:
        case NodeTag::kFloat:
        case NodeTag::kString:
        case NodeTag::kNull:
            outValue(buf, static_cast<const Value*>(node));
            break;
        case NodeTag::kVar:
            outVar(buf, static_cast<const Var*>(node));
            break;
        case NodeTag::kConst:
            outConst(buf, static_cast<const Const*>(node));
            break;
        case NodeTag::kParam:
            outParam(buf, static_cast<const Param*>(node));
            break;
        case NodeTag::kOpExpr:
            outOpExpr(buf, static_cast<const OpExpr*>(node));
            break;
        case NodeTag::kFuncExpr:
            outFuncExpr(buf, static_cast<const FuncExpr*>(node));
            break;
        case NodeTag::kBoolExpr:
            outBoolExpr(buf, static_cast<const BoolExpr*>(node));
            break;
        case NodeTag::kNullTest:
            outNullTest(buf, static_cast<const NullTest*>(node));
            break;
        case NodeTag::kBooleanTest:
            outBooleanTest(buf, static_cast<const BooleanTest*>(node));
            break;
        case NodeTag::kTargetEntry:
            outTargetEntry(buf, static_cast<const TargetEntry*>(node));
            break;
        case NodeTag::kRangeTblEntry:
            outRangeTblEntry(buf, static_cast<const RangeTblEntry*>(node));
            break;
        case NodeTag::kQuery:
            outQuery(buf, static_cast<const Query*>(node));
            break;
        default:
            // Unknown node type — just write the tag with no fields
            break;
    }

    buf->AppendChar(')');
}

// --- Per-type _out functions ---

static void outValue(StringInfo* buf, const Value* value) {
    switch (value->GetTag()) {
        case NodeTag::kInteger:
            buf->AppendPrintf(" :ival %lld", static_cast<long long>(value->GetInteger()));
            break;
        case NodeTag::kFloat:
            buf->AppendPrintf(" :fval %s", value->GetFloat().c_str());
            break;
        case NodeTag::kString:
            buf->AppendPrintf(" :sval %s", value->GetString().c_str());
            break;
        case NodeTag::kNull:
            // No value fields for null
            break;
        default:
            break;
    }
}

static void outVar(StringInfo* buf, const Var* node) {
    outInt(buf, "varno", node->varno);
    outInt(buf, "varattno", node->varattno);
    outOid(buf, "vartype", node->vartype);
    outInt(buf, "vartypmod", node->vartypmod);
    outOid(buf, "varcollid", node->varcollid);
    outInt(buf, "varlevelsup", node->varlevelsup);
    outInt(buf, "varnosyn", node->varnosyn);
    outInt(buf, "varattnosyn", node->varattnosyn);
    outInt(buf, "location", node->location);
}

static void outConst(StringInfo* buf, const Const* node) {
    outOid(buf, "consttype", node->consttype);
    outInt(buf, "consttypmod", node->consttypmod);
    outOid(buf, "constcollid", node->constcollid);
    outInt(buf, "constlen", node->constlen);
    outInt64(buf, "constvalue", static_cast<int64_t>(node->constvalue));
    outBool(buf, "constisnull", node->constisnull);
    outBool(buf, "constbyval", node->constbyval);
    outInt(buf, "location", node->location);
}

static void outParam(StringInfo* buf, const Param* node) {
    outInt(buf, "paramkind", static_cast<int>(node->paramkind));
    outInt(buf, "paramid", node->paramid);
    outOid(buf, "paramtype", node->paramtype);
    outInt(buf, "paramtypmod", node->paramtypmod);
    outOid(buf, "paramcollid", node->paramcollid);
    outInt(buf, "location", node->location);
}

static void outOpExpr(StringInfo* buf, const OpExpr* node) {
    outOid(buf, "opno", node->opno);
    outOid(buf, "opfuncid", node->opfuncid);
    outOid(buf, "opresulttype", node->opresulttype);
    outBool(buf, "opretset", node->opretset);
    outOid(buf, "opcollid", node->opcollid);
    outOid(buf, "inputcollid", node->inputcollid);
    outNodeList(buf, "args", node->args);
    outInt(buf, "location", node->location);
}

static void outFuncExpr(StringInfo* buf, const FuncExpr* node) {
    outOid(buf, "funcid", node->funcid);
    outOid(buf, "funcresulttype", node->funcresulttype);
    outBool(buf, "funcretset", node->funcretset);
    outBool(buf, "funcvariadic", node->funcvariadic);
    outInt(buf, "funcformat", static_cast<int>(node->funcformat));
    outOid(buf, "funccollid", node->funccollid);
    outOid(buf, "inputcollid", node->inputcollid);
    outNodeList(buf, "args", node->args);
    outInt(buf, "location", node->location);
}

static void outBoolExpr(StringInfo* buf, const BoolExpr* node) {
    outInt(buf, "boolop", static_cast<int>(node->boolop));
    outNodeList(buf, "args", node->args);
    outInt(buf, "location", node->location);
}

static void outNullTest(StringInfo* buf, const NullTest* node) {
    outNodeField(buf, "arg", node->arg);
    outInt(buf, "nulltesttype", static_cast<int>(node->nulltesttype));
    outBool(buf, "argisrow", node->argisrow);
    outInt(buf, "location", node->location);
}

static void outBooleanTest(StringInfo* buf, const BooleanTest* node) {
    outNodeField(buf, "arg", node->arg);
    outInt(buf, "booltesttype", static_cast<int>(node->booltesttype));
    outInt(buf, "location", node->location);
}

static void outTargetEntry(StringInfo* buf, const TargetEntry* node) {
    outNodeField(buf, "expr", node->expr);
    outInt(buf, "resno", node->resno);
    outString(buf, "resname", node->resname);
    outInt(buf, "ressortgroupref", node->ressortgroupref);
    outOid(buf, "resorigtbl", node->resorigtbl);
    outInt(buf, "resorigcol", node->resorigcol);
    outBool(buf, "resjunk", node->resjunk);
}

static void outRangeTblEntry(StringInfo* buf, const RangeTblEntry* node) {
    outInt(buf, "rtekind", static_cast<int>(node->rtekind));
    outOid(buf, "relid", node->relid);
    outChar(buf, "relkind", node->relkind);
    outInt(buf, "rellockmode", node->rellockmode);
    outNodeField(buf, "tablesample", reinterpret_cast<const Node*>(node->tablesample));
    outNodeField(buf, "subquery", reinterpret_cast<const Node*>(node->subquery));
    outBool(buf, "security_barrier", node->security_barrier);
    outInt(buf, "jointype", static_cast<int>(node->jointype));
    outInt(buf, "joinmergedcols", node->joinmergedcols);
    outNodeList(buf, "joinaliasvars", node->joinaliasvars);
    outNodeList(buf, "joinleftcols", node->joinleftcols);
    outNodeList(buf, "joinrightcols", node->joinrightcols);
    outNodeList(buf, "functions", node->functions);
    outBool(buf, "funcordinality", node->funcordinality);
    // values_lists is a vector of vectors; emit just a count marker. The
    // nested expression structure is not round-tripped through this textual
    // format (no current callers depend on it). Readers skip unknown fields.
    buf->AppendPrintf(" :values_lists_count %zu", node->values_lists.size());
    outString(buf, "ctename", node->ctename);
    outInt(buf, "ctelevelsup", node->ctelevelsup);
    outBool(buf, "self_reference", node->self_reference);
    outNodeList(buf, "coltypes", node->coltypes);
    outNodeList(buf, "coltypmods", node->coltypmods);
    outNodeList(buf, "colcollations", node->colcollations);
}

static void outQuery(StringInfo* buf, const Query* node) {
    outInt(buf, "commandType", static_cast<int>(node->command_type));
    outInt(buf, "querySource", static_cast<int>(node->query_source));
    outInt64(buf, "queryId", node->query_id);
    outBool(buf, "canSetTag", node->can_set_tag);
    outNodeField(buf, "utilityStmt", node->utility_stmt);
    outInt(buf, "resultRelation", node->result_relation);
    outBool(buf, "hasAggs", node->has_aggs);
    outBool(buf, "hasWindowFuncs", node->has_window_funcs);
    outBool(buf, "hasTargetSRFs", node->has_target_srfs);
    outBool(buf, "hasSubLinks", node->has_sub_links);
    outBool(buf, "hasDistinctOn", node->has_distinct_on);
    outBool(buf, "hasRecursive", node->has_recursive);
    outBool(buf, "hasModifyingCTE", node->has_modifying_cte);
    outBool(buf, "hasForUpdate", node->has_for_update);
    outBool(buf, "hasRowSecurity", node->has_row_security);
    outBool(buf, "isReturn", node->is_return);
    outNodeList(buf, "cteList", node->cte_list);
    outNodeList(buf, "rtable", node->rtable);
    outNodeField(buf, "jointree", node->jointree);
    outNodeList(buf, "targetList", node->target_list);
    outInt(buf, "override", static_cast<int>(node->override_kind));
    outNodeField(buf, "onConflict", node->on_conflict);
    outNodeList(buf, "returningList", node->returning_list);
    outNodeList(buf, "groupClause", node->group_clause);
    outBool(buf, "groupDistinct", node->group_distinct);
    outNodeField(buf, "havingQual", node->having_qual);
    outNodeList(buf, "windowClause", node->window_clause);
    outNodeList(buf, "distinctClause", node->distinct_clause);
    outNodeList(buf, "sortClause", node->sort_clause);
    outNodeField(buf, "limitOffset", node->limit_offset);
    outNodeField(buf, "limitCount", node->limit_count);
}

// --- Public API ---

char* nodeToString(const Node* node) {
    if (node == nullptr) {
        return nullptr;
    }
    StringInfo buf;
    outNode(&buf, node);
    // Return a palloc'd copy (PG API contract: caller pfree's the result).
    auto* result = static_cast<char*>(pgcpp::memory::palloc(buf.Length() + 1));
    std::memcpy(result, buf.Data(), buf.Length() + 1);
    return result;
}

std::string nodeToStdString(const Node* node) {
    if (node == nullptr) {
        return {};
    }
    StringInfo buf;
    outNode(&buf, node);
    return buf.Str();
}

}  // namespace pgcpp::nodes
