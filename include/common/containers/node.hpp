#pragma once

#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::nodes {

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
    // Statement node types (Phase 5 grammar expansion)
    kTransactionStmt,
    kTruncateStmt,
    kExplainStmt,
    kCommentStmt,
    kIndexStmt,
    kViewStmt,
    kCreateAsStmt,
    kVacuumStmt,
    kVariableSetStmt,
    kClusterStmt,
    kLockStmt,
    kDiscardStmt,
    kNotifyStmt,
    kListenStmt,
    kUnlistenStmt,
    kCheckPointStmt,
    kReindexStmt,
    kDeallocateStmt,
    kPrepareStmt,
    kExecuteStmt,
    kLoadStmt,
    kCallStmt,
    kRenameStmt,
    kAlterOwnerStmt,
    kCreateSeqStmt,
    kAlterSeqStmt,
    kCreateFunctionStmt,
    kAlterFunctionStmt,
    kDropFunctionStmt,
    kCreateTrigStmt,
    kCreateRoleStmt,
    kAlterRoleStmt,
    kDropRoleStmt,
    kGrantStmt,
    kGrantRoleStmt,
    kCopyStmt,
    kRefreshMatViewStmt,
    kCreateTableSpaceStmt,
    kDropTableSpaceStmt,
    kCreatedbStmt,
    kDropdbStmt,
    kAlterDatabaseStmt,
    // Transformed expression node types (primnodes.h)
    kAggref,
    kNullTest,
    kCaseExpr,
    kCaseWhen,
    kSubLink,
    kRangeTblRef,
    kJoinExpr,
    kFromExpr,
    kRelabelType,
    kCoerceViaIO,
    kScalarArrayOpExpr,
    kFieldSelect,
    kRowExpr,
    kCoerceToDomain,
    kBooleanTest,
    kCoerceToDomainValue,
    kSetToDefault,
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

// makePallocNode — allocate a node in the current memory context via palloc,
// construct it with placement new, and register its destructor so that
// std::string/std::vector members (allocated via operator new) are properly
// released when the owning MemoryContext is deleted. This is the canonical
// replacement for the `palloc + placement new` pattern; without
// RegisterDestructor, C++ members would leak when the context is freed
// (longjmp-based ereport and bulk context reset both bypass C++ destructors).
template<typename T, typename... Args>
T* makePallocNode(Args&&... args) {
    void* mem = pgcpp::memory::palloc(sizeof(T));
    T* obj = new (mem) T(std::forward<Args>(args)...);
    pgcpp::memory::MemoryContext* ctx = pgcpp::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(obj, [](void* p) { static_cast<T*>(p)->~T(); });
    }
    return obj;
}

// destroyPallocNode — explicitly destroy a palloc'd C++ object: unregister
// its destructor (to prevent double-free when the MemoryContext is later
// deleted), call the destructor explicitly, then pfree the memory. Use this
// for objects with explicit lifetimes (e.g., ParseState, EState) that are
// destroyed before their MemoryContext is deleted.
template<typename T>
void destroyPallocNode(T* obj) {
    if (obj == nullptr)
        return;
    // Use the owning context from the chunk header, not CurrentMemoryContext,
    // so this works even when the current context is null or different.
    pgcpp::memory::MemoryContext* ctx = pgcpp::memory::AllocSetContext::GetPointerContext(obj);
    if (ctx != nullptr) {
        ctx->UnregisterDestructor(obj);
    }
    obj->~T();
    pgcpp::memory::pfree(obj);
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

}  // namespace pgcpp::nodes
