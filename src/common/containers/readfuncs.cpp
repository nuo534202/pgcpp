// readfuncs.cpp — node tree deserialization (string → Node).
//
// Converted from PostgreSQL 15's src/backend/nodes/readfuncs.c.
//
// Implements stringToNode() and per-node-type _read functions for core
// node types. Parses the PG-format S-expression produced by outfuncs.
//
// See outfuncs.hpp for format details.
#include "common/containers/readfuncs.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

namespace pgcpp::nodes {

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

// ---------------------------------------------------------------------------
// ReadContext — cursor-based parser for S-expression strings.
// ---------------------------------------------------------------------------

struct ReadContext {
    const char* data;
    std::size_t pos;
    std::size_t len;

    explicit ReadContext(const char* str) : data(str), pos(0), len(std::strlen(str)) {}

    char peek() const { return pos < len ? data[pos] : '\0'; }

    char advance() { return pos < len ? data[pos++] : '\0'; }

    void skipWhitespace() {
        while (pos < len) {
            char c = data[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    // Read a token: sequence of chars until whitespace, paren, or end.
    std::string readToken() {
        skipWhitespace();
        std::string tok;
        while (pos < len) {
            char c = data[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')') {
                break;
            }
            tok += c;
            ++pos;
        }
        return tok;
    }

    // Read a field label: ":fieldname"
    std::string readFieldLabel() {
        skipWhitespace();
        std::string tok;
        if (pos < len && data[pos] == ':') {
            ++pos;
            while (pos < len) {
                char c = data[pos];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    break;
                }
                tok += c;
                ++pos;
            }
        }
        return tok;
    }

    // Check if the next value is null "<>".
    bool isNullValue() {
        skipWhitespace();
        return pos + 1 < len && data[pos] == '<' && data[pos + 1] == '>';
    }

    // Consume a null value "<>".
    void consumeNull() { pos += 2; }
};

// --- Forward declarations ---

static Node* readNode(ReadContext& ctx);
static Node* readValueNode(const std::string& tag, ReadContext& ctx);

// --- Value readers ---

static int readIntValue(ReadContext& ctx) {
    std::string tok = ctx.readToken();
    return std::atoi(tok.c_str());
}

static int64_t readInt64Value(ReadContext& ctx) {
    std::string tok = ctx.readToken();
    return static_cast<int64_t>(std::atoll(tok.c_str()));
}

static bool readBoolValue(ReadContext& ctx) {
    std::string tok = ctx.readToken();
    return tok == "t";
}

static unsigned int readOidValue(ReadContext& ctx) {
    std::string tok = ctx.readToken();
    return static_cast<unsigned int>(std::strtoul(tok.c_str(), nullptr, 10));
}

static char readCharValue(ReadContext& ctx) {
    std::string tok = ctx.readToken();
    if (tok.empty())
        return 0;
    if (tok == "0")
        return 0;
    return tok[0];
}

static std::string readStringValue(ReadContext& ctx) {
    ctx.skipWhitespace();
    // outString serializes empty strings as "<>" (same marker as null node
    // fields). For std::string fields there is no null concept, so map "<>"
    // back to an empty string.
    if (ctx.isNullValue()) {
        ctx.consumeNull();
        return "";
    }
    return ctx.readToken();
}

// Read a node field value: either null (<>), a nested node (TAG ...),
// or a list of nodes ((item1 item2 ...)).
static Node* readNodeFieldValue(ReadContext& ctx) {
    ctx.skipWhitespace();
    if (ctx.isNullValue()) {
        ctx.consumeNull();
        return nullptr;
    }
    return readNode(ctx);
}

// Read a list of nodes: (item1 item2 ...) or () for empty.
static std::vector<Node*> readNodeListValue(ReadContext& ctx) {
    std::vector<Node*> result;
    ctx.skipWhitespace();
    if (ctx.peek() != '(') {
        // Not a list — try reading a single null
        if (ctx.isNullValue()) {
            ctx.consumeNull();
        }
        return result;
    }
    ctx.advance();  // consume '('
    ctx.skipWhitespace();
    while (ctx.peek() != ')' && ctx.peek() != '\0') {
        if (ctx.isNullValue()) {
            ctx.consumeNull();
            ctx.skipWhitespace();
            continue;
        }
        Node* item = readNode(ctx);
        if (item != nullptr) {
            result.push_back(item);
        }
        ctx.skipWhitespace();
    }
    if (ctx.peek() == ')') {
        ctx.advance();  // consume ')'
    }
    return result;
}

// --- Tag name → NodeTag mapping ---

static NodeTag parseTag(const std::string& tag) {
    if (tag == "VAR")
        return NodeTag::kVar;
    if (tag == "CONST")
        return NodeTag::kConst;
    if (tag == "PARAM")
        return NodeTag::kParam;
    if (tag == "OPEXPR")
        return NodeTag::kOpExpr;
    if (tag == "FUNCEXPR")
        return NodeTag::kFuncExpr;
    if (tag == "BOOLEXPR")
        return NodeTag::kBoolExpr;
    if (tag == "NULLTEST")
        return NodeTag::kNullTest;
    if (tag == "BOOLEANTEST")
        return NodeTag::kBooleanTest;
    if (tag == "TARGETENTRY")
        return NodeTag::kTargetEntry;
    if (tag == "RTE")
        return NodeTag::kRangeTblEntry;
    if (tag == "QUERY")
        return NodeTag::kQuery;
    if (tag == "INTEGER")
        return NodeTag::kInteger;
    if (tag == "FLOAT")
        return NodeTag::kFloat;
    if (tag == "STRING")
        return NodeTag::kString;
    if (tag == "NULL")
        return NodeTag::kNull;
    return NodeTag::kInvalid;
}

// --- Per-type _read functions ---

static Node* readVar(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<Var>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "varno")
            node->varno = readIntValue(ctx);
        else if (field == "varattno")
            node->varattno = readIntValue(ctx);
        else if (field == "vartype")
            node->vartype = readOidValue(ctx);
        else if (field == "vartypmod")
            node->vartypmod = readIntValue(ctx);
        else if (field == "varcollid")
            node->varcollid = readOidValue(ctx);
        else if (field == "varlevelsup")
            node->varlevelsup = readIntValue(ctx);
        else if (field == "varnosyn")
            node->varnosyn = readIntValue(ctx);
        else if (field == "varattnosyn")
            node->varattnosyn = readIntValue(ctx);
        else if (field == "location")
            node->location = readIntValue(ctx);
        else
            ctx.readToken();  // skip unknown value
    }
    return node;
}

static Node* readConst(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<Const>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "consttype")
            node->consttype = readOidValue(ctx);
        else if (field == "consttypmod")
            node->consttypmod = readIntValue(ctx);
        else if (field == "constcollid")
            node->constcollid = readOidValue(ctx);
        else if (field == "constlen")
            node->constlen = readIntValue(ctx);
        else if (field == "constvalue")
            node->constvalue = static_cast<pgcpp::types::Datum>(readInt64Value(ctx));
        else if (field == "constisnull")
            node->constisnull = readBoolValue(ctx);
        else if (field == "constbyval")
            node->constbyval = readBoolValue(ctx);
        else if (field == "location")
            node->location = readIntValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readParam(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<Param>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "paramkind")
            node->paramkind = static_cast<pgcpp::parser::ParamKind>(readIntValue(ctx));
        else if (field == "paramid")
            node->paramid = readIntValue(ctx);
        else if (field == "paramtype")
            node->paramtype = readOidValue(ctx);
        else if (field == "paramtypmod")
            node->paramtypmod = readIntValue(ctx);
        else if (field == "paramcollid")
            node->paramcollid = readOidValue(ctx);
        else if (field == "location")
            node->location = readIntValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readOpExpr(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<OpExpr>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "opno")
            node->opno = readOidValue(ctx);
        else if (field == "opfuncid")
            node->opfuncid = readOidValue(ctx);
        else if (field == "opresulttype")
            node->opresulttype = readOidValue(ctx);
        else if (field == "opretset")
            node->opretset = readBoolValue(ctx);
        else if (field == "opcollid")
            node->opcollid = readOidValue(ctx);
        else if (field == "inputcollid")
            node->inputcollid = readOidValue(ctx);
        else if (field == "args")
            node->args = readNodeListValue(ctx);
        else if (field == "location")
            node->location = readIntValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readFuncExpr(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<FuncExpr>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "funcid")
            node->funcid = readOidValue(ctx);
        else if (field == "funcresulttype")
            node->funcresulttype = readOidValue(ctx);
        else if (field == "funcretset")
            node->funcretset = readBoolValue(ctx);
        else if (field == "funcvariadic")
            node->funcvariadic = readBoolValue(ctx);
        else if (field == "funcformat")
            node->funcformat = static_cast<pgcpp::parser::CoercionForm>(readIntValue(ctx));
        else if (field == "funccollid")
            node->funccollid = readOidValue(ctx);
        else if (field == "inputcollid")
            node->inputcollid = readOidValue(ctx);
        else if (field == "args")
            node->args = readNodeListValue(ctx);
        else if (field == "location")
            node->location = readIntValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readBoolExpr(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<BoolExpr>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "boolop")
            node->boolop = static_cast<pgcpp::parser::BoolExprType>(readIntValue(ctx));
        else if (field == "args")
            node->args = readNodeListValue(ctx);
        else if (field == "location")
            node->location = readIntValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readNullTest(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<NullTest>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "arg")
            node->arg = readNodeFieldValue(ctx);
        else if (field == "nulltesttype")
            node->nulltesttype = static_cast<pgcpp::parser::NullTestType>(readIntValue(ctx));
        else if (field == "argisrow")
            node->argisrow = readBoolValue(ctx);
        else if (field == "location")
            node->location = readIntValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readBooleanTest(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<BooleanTest>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "arg")
            node->arg = readNodeFieldValue(ctx);
        else if (field == "booltesttype")
            node->booltesttype = static_cast<pgcpp::parser::BoolTestType>(readIntValue(ctx));
        else if (field == "location")
            node->location = readIntValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readTargetEntry(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<TargetEntry>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "expr")
            node->expr = readNodeFieldValue(ctx);
        else if (field == "resno")
            node->resno = readIntValue(ctx);
        else if (field == "resname")
            node->resname = readStringValue(ctx);
        else if (field == "ressortgroupref")
            node->ressortgroupref = readIntValue(ctx);
        else if (field == "resorigtbl")
            node->resorigtbl = readOidValue(ctx);
        else if (field == "resorigcol")
            node->resorigcol = readIntValue(ctx);
        else if (field == "resjunk")
            node->resjunk = readBoolValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readRangeTblEntry(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<RangeTblEntry>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "rtekind")
            node->rtekind = static_cast<pgcpp::parser::RTEKind>(readIntValue(ctx));
        else if (field == "relid")
            node->relid = readIntValue(ctx);
        else if (field == "relkind")
            node->relkind = readCharValue(ctx);
        else if (field == "rellockmode")
            node->rellockmode = readIntValue(ctx);
        else if (field == "subquery")
            node->subquery = reinterpret_cast<pgcpp::parser::Query*>(readNodeFieldValue(ctx));
        else if (field == "security_barrier")
            node->security_barrier = readBoolValue(ctx);
        else if (field == "jointype")
            node->jointype = static_cast<pgcpp::parser::JoinType>(readIntValue(ctx));
        else if (field == "joinmergedcols")
            node->joinmergedcols = readIntValue(ctx);
        else if (field == "joinaliasvars")
            node->joinaliasvars = readNodeListValue(ctx);
        else if (field == "joinleftcols")
            node->joinleftcols = readNodeListValue(ctx);
        else if (field == "joinrightcols")
            node->joinrightcols = readNodeListValue(ctx);
        else if (field == "functions")
            node->functions = readNodeListValue(ctx);
        else if (field == "funcordinality")
            node->funcordinality = readBoolValue(ctx);
        else if (field == "values_lists")
            node->values_lists = readNodeListValue(ctx);
        else if (field == "ctename")
            node->ctename = readStringValue(ctx);
        else if (field == "ctelevelsup")
            node->ctelevelsup = readIntValue(ctx);
        else if (field == "self_reference")
            node->self_reference = readBoolValue(ctx);
        else if (field == "coltypes")
            node->coltypes = readNodeListValue(ctx);
        else if (field == "coltypmods")
            node->coltypmods = readNodeListValue(ctx);
        else if (field == "colcollations")
            node->colcollations = readNodeListValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

static Node* readQuery(ReadContext& ctx) {
    auto* node = pgcpp::nodes::makePallocNode<Query>();
    while (true) {
        ctx.skipWhitespace();
        if (ctx.peek() == ')' || ctx.peek() == '\0')
            break;
        std::string field = ctx.readFieldLabel();
        if (field.empty())
            break;
        if (field == "commandType")
            node->command_type = static_cast<pgcpp::parser::CmdType>(readIntValue(ctx));
        else if (field == "querySource")
            node->query_source = static_cast<pgcpp::parser::QuerySource>(readIntValue(ctx));
        else if (field == "queryId")
            node->query_id = readInt64Value(ctx);
        else if (field == "canSetTag")
            node->can_set_tag = readBoolValue(ctx);
        else if (field == "utilityStmt")
            node->utility_stmt = readNodeFieldValue(ctx);
        else if (field == "resultRelation")
            node->result_relation = readIntValue(ctx);
        else if (field == "hasAggs")
            node->has_aggs = readBoolValue(ctx);
        else if (field == "hasWindowFuncs")
            node->has_window_funcs = readBoolValue(ctx);
        else if (field == "hasTargetSRFs")
            node->has_target_srfs = readBoolValue(ctx);
        else if (field == "hasSubLinks")
            node->has_sub_links = readBoolValue(ctx);
        else if (field == "hasDistinctOn")
            node->has_distinct_on = readBoolValue(ctx);
        else if (field == "hasRecursive")
            node->has_recursive = readBoolValue(ctx);
        else if (field == "hasModifyingCTE")
            node->has_modifying_cte = readBoolValue(ctx);
        else if (field == "hasForUpdate")
            node->has_for_update = readBoolValue(ctx);
        else if (field == "hasRowSecurity")
            node->has_row_security = readBoolValue(ctx);
        else if (field == "isReturn")
            node->is_return = readBoolValue(ctx);
        else if (field == "cteList")
            node->cte_list = readNodeListValue(ctx);
        else if (field == "rtable")
            node->rtable = readNodeListValue(ctx);
        else if (field == "jointree")
            node->jointree = readNodeFieldValue(ctx);
        else if (field == "targetList")
            node->target_list = readNodeListValue(ctx);
        else if (field == "override")
            node->override_kind = static_cast<pgcpp::parser::OverridingKind>(readIntValue(ctx));
        else if (field == "onConflict")
            node->on_conflict = readNodeFieldValue(ctx);
        else if (field == "returningList")
            node->returning_list = readNodeListValue(ctx);
        else if (field == "groupClause")
            node->group_clause = readNodeListValue(ctx);
        else if (field == "groupDistinct")
            node->group_distinct = readBoolValue(ctx);
        else if (field == "havingQual")
            node->having_qual = readNodeFieldValue(ctx);
        else if (field == "windowClause")
            node->window_clause = readNodeListValue(ctx);
        else if (field == "distinctClause")
            node->distinct_clause = readNodeListValue(ctx);
        else if (field == "sortClause")
            node->sort_clause = readNodeListValue(ctx);
        else if (field == "limitOffset")
            node->limit_offset = readNodeFieldValue(ctx);
        else if (field == "limitCount")
            node->limit_count = readNodeFieldValue(ctx);
        else
            ctx.readToken();
    }
    return node;
}

// --- Value node reader ---

static Node* readValueNode(const std::string& tag, ReadContext& ctx) {
    if (tag == "INTEGER") {
        std::string field = ctx.readFieldLabel();  // "ival"
        int64_t val = readInt64Value(ctx);
        return pgcpp::nodes::makePallocNode<Value>(val);
    }
    if (tag == "FLOAT") {
        std::string field = ctx.readFieldLabel();  // "fval"
        std::string val = readStringValue(ctx);
        return pgcpp::nodes::makePallocNode<Value>(val);
    }
    if (tag == "STRING") {
        std::string field = ctx.readFieldLabel();  // "sval"
        std::string val = readStringValue(ctx);
        return pgcpp::nodes::makePallocNode<Value>(val, true);
    }
    if (tag == "NULL") {
        return pgcpp::nodes::makePallocNode<Value>();
    }
    return nullptr;
}

// --- Main dispatch ---

static Node* readNode(ReadContext& ctx) {
    ctx.skipWhitespace();
    if (ctx.peek() != '(') {
        // Not a node — skip token
        ctx.readToken();
        return nullptr;
    }
    ctx.advance();  // consume '('

    std::string tag = ctx.readToken();
    NodeTag tagEnum = parseTag(tag);

    Node* result = nullptr;
    switch (tagEnum) {
        case NodeTag::kVar:
            result = readVar(ctx);
            break;
        case NodeTag::kConst:
            result = readConst(ctx);
            break;
        case NodeTag::kParam:
            result = readParam(ctx);
            break;
        case NodeTag::kOpExpr:
            result = readOpExpr(ctx);
            break;
        case NodeTag::kFuncExpr:
            result = readFuncExpr(ctx);
            break;
        case NodeTag::kBoolExpr:
            result = readBoolExpr(ctx);
            break;
        case NodeTag::kNullTest:
            result = readNullTest(ctx);
            break;
        case NodeTag::kBooleanTest:
            result = readBooleanTest(ctx);
            break;
        case NodeTag::kTargetEntry:
            result = readTargetEntry(ctx);
            break;
        case NodeTag::kRangeTblEntry:
            result = readRangeTblEntry(ctx);
            break;
        case NodeTag::kQuery:
            result = readQuery(ctx);
            break;
        case NodeTag::kInteger:
        case NodeTag::kFloat:
        case NodeTag::kString:
        case NodeTag::kNull:
            result = readValueNode(tag, ctx);
            break;
        default:
            // Unknown tag — skip to closing paren
            break;
    }

    // Skip to matching ')'
    int depth = 1;
    while (ctx.pos < ctx.len && depth > 0) {
        char c = ctx.advance();
        if (c == '(')
            ++depth;
        else if (c == ')')
            --depth;
    }

    return result;
}

// --- Public API ---

Node* stringToNode(const char* str) {
    if (str == nullptr || str[0] == '\0') {
        return nullptr;
    }
    ReadContext ctx(str);
    return readNode(ctx);
}

}  // namespace pgcpp::nodes
