#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mytoydb::nodes {

// NodeTag — enumeration of all node types. Kept for PostgreSQL API
// compatibility (nodeTag() macro). In C++ we also use virtual functions
// and dynamic_cast/static_cast for type dispatch.
enum class NodeTag : int {
    kInvalid = 0,
    // Value types
    kInteger,
    kFloat,
    kString,
    kBitString,
    kNull,
    // Expression types (subset needed for later phases)
    kExpr,
    kVar,
    kConst,
    kParam,
    kOpExpr,
    kFuncExpr,
    kBoolExpr,
    // Statement types (subset)
    kRawStmt,
    kSelectStmt,
    kInsertStmt,
    kUpdateStmt,
    kDeleteStmt,
    kCreateStmt,
    // List type
    kList,
    // Target entry
    kTargetEntry,
    // Range var (table reference)
    kRangeVar,
    // Column reference
    kColumnRef,
    // A_Expr (ambiguous expression)
    kAExpr,
    kAConst,
    kAStar,
    // Type name
    kTypeName,
    // Alias
    kAlias,
    // Parser node types (parsenodes.h)
    kParamRef,
    kTypeCast,
    kCollateClause,
    kRoleSpec,
    kFuncCall,
    kAIndices,
    kAIndirection,
    kAArrayExpr,
    kResTarget,
    kMultiAssignRef,
    kSortBy,
    kWindowDef,
    kRangeSubselect,
    kRangeFunction,
    kColumnDef,
    kIndexElem,
    kDefElem,
    kLockingClause,
    kXmlSerialize,
    kPartitionElem,
    kPartitionSpec,
    kPartitionBoundSpec,
    kPartitionRangeDatum,
    kPartitionCmd,
    kInferClause,
    kOnConflictClause,
    kCommonTableExpr,
    kMergeWhenClause,
    kMergeAction,
    kTriggerTransition,
    kAccessPriv,
    kIntoClause,
    kWithClause,
    kCreateSchemaStmt,
    kAlterTableStmt,
    kAlterTableCmd,
    kDropStmt,
    kRangeTblEntry,
    kQuery,
    kSortGroupClause,
    kGroupingSet,
    kWindowClause,
    kRowMarkClause,
    kWithCheckOption,
    kRangeTblFunction,
    kTableSampleClause,
};

// Node — the abstract base class for all AST nodes.
// In PostgreSQL C, this is just a NodeTag field; all node structs start with
// NodeTag. In C++, we use inheritance + a virtual destructor + a tag field.
class Node {
public:
    virtual ~Node() = default;

    NodeTag GetTag() const { return tag_; }

    // Virtual deep-copy. Each concrete subclass overrides this to return a
    // heap-allocated copy of itself. The caller owns the result.
    // Allocations use the current memory context (palloc).
    virtual Node* Clone() const = 0;

    // Virtual deep-equality. Each concrete subclass overrides this to
    // compare its fields.
    virtual bool Equals(const Node& other) const = 0;

protected:
    explicit Node(NodeTag tag) : tag_(tag) {}

private:
    NodeTag tag_;
};

// nodeTag() — PostgreSQL-compatible accessor. Returns the NodeTag.
inline NodeTag nodeTag(const Node* node) {
    return node ? node->GetTag() : NodeTag::kInvalid;
}

// isA() — PostgreSQL-compatible type check.
inline bool isA(const Node* node, NodeTag tag) {
    return node != nullptr && node->GetTag() == tag;
}

// copyObject() — PostgreSQL-compatible deep copy. Returns a new Node*.
Node* copyObject(const Node* node);

// equal() — PostgreSQL-compatible deep equality.
bool equal(const Node* a, const Node* b);

// Value — literal value node (PostgreSQL's Value struct).
// Represents integer, float, string, or null constants in the parser.
class Value : public Node {
public:
    // Construct from integer
    explicit Value(int64_t ival);
    // Construct from float (stored as string, like PostgreSQL)
    explicit Value(std::string fval);
    // Construct from string
    explicit Value(std::string sval, bool is_string);
    // Construct null
    Value();

    int64_t GetInteger() const { return ival_; }
    const std::string& GetFloat() const { return sval_; }
    const std::string& GetString() const { return sval_; }
    bool IsNull() const { return GetTag() == NodeTag::kNull; }

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

private:
    int64_t ival_ = 0;
    std::string sval_;
};

// Convenience constructors (PostgreSQL-style makeInteger/makeString/etc.)
Value* makeInteger(int64_t ival);
Value* makeFloat(std::string fval);
Value* makeString(std::string sval);
Value* makeNull();

}  // namespace mytoydb::nodes
