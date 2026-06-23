// parsenodes.h — C++ versions of PostgreSQL's parse tree node types.
//
// Converted from PostgreSQL 15's src/include/nodes/parsenodes.h.
// All node types inherit from mytoydb::nodes::Node and provide Clone() /
// Equals() overrides. String fields use std::string, List* fields use
// std::vector<Node*>, and Oid/int32 fields use int.
//
// NodeTag additions needed in node.h (already added):
//   kParamRef, kTypeCast, kCollateClause, kRoleSpec, kFuncCall,
//   kAIndices, kAIndirection, kAArrayExpr, kResTarget, kMultiAssignRef,
//   kSortBy, kWindowDef, kRangeSubselect, kRangeFunction, kColumnDef,
//   kIndexElem, kDefElem, kLockingClause, kXmlSerialize, kPartitionElem,
//   kPartitionSpec, kPartitionBoundSpec, kPartitionRangeDatum, kPartitionCmd,
//   kInferClause, kOnConflictClause, kCommonTableExpr, kMergeWhenClause,
//   kMergeAction, kTriggerTransition, kAccessPriv, kIntoClause, kWithClause,
//   kCreateSchemaStmt, kAlterTableStmt, kAlterTableCmd, kDropStmt,
//   kRangeTblEntry, kQuery, kSortGroupClause, kGroupingSet, kWindowClause,
//   kRowMarkClause, kWithCheckOption, kRangeTblFunction, kTableSampleClause.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mytoydb/common/containers/node.h"

namespace mytoydb::parser {

using mytoydb::nodes::Node;

// ---------------------------------------------------------------------------
// Enums from other PostgreSQL headers (defined here until those modules are
// converted). These mirror CoercionForm, LockClauseStrength, LockWaitPolicy,
// XmlOptionType, OnConflictAction, LimitOption, JoinType, CmdType,
// OnCommitAction from primnodes.h / lockoptions.h / nodes.h.
// ---------------------------------------------------------------------------

enum class CoercionForm {
    kImplicit,
    kAssignmentCast,
    kExplicit,
};

enum class LockClauseStrength {
    kNone,
    kForKeyShare,
    kForShare,
    kForNoKeyUpdate,
    kForUpdate,
};

enum class LockWaitPolicy {
    kBlock,
    kSkip,
    kError,
};

enum class XmlOptionType {
    kDocument,
    kContent,
};

enum class OnConflictAction {
    kNone,
    kNothing,
    kUpdate,
};

enum class LimitOption {
    kCount,
    kWithTies,
};

enum class JoinType {
    kInner,
    kLeft,
    kFull,
    kRight,
    kSemi,
    kAnti,
    kUniqueOuter,
    kUniqueInner,
};

enum class CmdType {
    kUnknown,
    kSelect,
    kInsert,
    kUpdate,
    kDelete,
    kMerge,
    kUtility,
    kNothing,
};

enum class OnCommitAction {
    kNoop,
    kPreserveRows,
    kDeleteRows,
    kDrop,
};

// ---------------------------------------------------------------------------
// Parse-node enums (from parsenodes.h)
// ---------------------------------------------------------------------------

enum class OverridingKind {
    kNotSet = 0,
    kUserValue,
    kSystemValue,
};

enum class QuerySource {
    kOriginal,
    kParser,
    kInsteadRule,
    kQualInsteadRule,
    kNonInsteadRule,
};

enum class SortByDir {
    kDefault,
    kAsc,
    kDesc,
    kUsing,
};

enum class SortByNulls {
    kDefault,
    kFirst,
    kLast,
};

enum class SetQuantifier {
    kDefault,
    kAll,
    kDistinct,
};

enum class SetOperation {
    kNone = 0,
    kUnion,
    kIntersect,
    kExcept,
};

enum class ObjectType {
    kAccessMethod,
    kAggregate,
    kAmop,
    kAmproc,
    kAttribute,
    kCast,
    kColumn,
    kCollation,
    kConversion,
    kDatabase,
    kDefault,
    kDefacl,
    kDomain,
    kDomconstraint,
    kEventTrigger,
    kExtension,
    kFdw,
    kForeignServer,
    kForeignTable,
    kFunction,
    kIndex,
    kLanguage,
    kLargeobject,
    kMatview,
    kOpclass,
    kOperator,
    kOpfamily,
    kParameterAcl,
    kPolicy,
    kProcedure,
    kPublication,
    kPublicationNamespace,
    kPublicationRel,
    kRole,
    kRoutine,
    kRule,
    kSchema,
    kSequence,
    kSubscription,
    kStatisticExt,
    kTabconstraint,
    kTable,
    kTablespace,
    kTransform,
    kTrigger,
    kTsconfiguration,
    kTsdictionary,
    kTsparser,
    kTstemplate,
    kType,
    kUserMapping,
    kView,
};

enum class DropBehavior {
    kRestrict,
    kCascade,
};

enum class AlterTableType {
    kAddColumn,
    kAddColumnRecurse,
    kAddColumnToView,
    kColumnDefault,
    kCookedColumnDefault,
    kDropNotNull,
    kSetNotNull,
    kDropExpression,
    kCheckNotNull,
    kSetStatistics,
    kSetOptions,
    kResetOptions,
    kSetStorage,
    kSetCompression,
    kDropColumn,
    kDropColumnRecurse,
    kAddIndex,
    kReAddIndex,
    kAddConstraint,
    kAddConstraintRecurse,
    kReAddConstraint,
    kReAddDomainConstraint,
    kAlterConstraint,
    kValidateConstraint,
    kValidateConstraintRecurse,
    kAddIndexConstraint,
    kDropConstraint,
    kDropConstraintRecurse,
    kReAddComment,
    kAlterColumnType,
    kAlterColumnGenericOptions,
    kChangeOwner,
    kClusterOn,
    kDropCluster,
    kSetLogged,
    kSetUnlogged,
    kDropOids,
    kSetAccessMethod,
    kSetTableSpace,
    kSetRelOptions,
    kResetRelOptions,
    kReplaceRelOptions,
    kEnableTrig,
    kEnableAlwaysTrig,
    kEnableReplicaTrig,
    kDisableTrig,
    kEnableTrigAll,
    kDisableTrigAll,
    kEnableTrigUser,
    kDisableTrigUser,
    kEnableRule,
    kEnableAlwaysRule,
    kEnableReplicaRule,
    kDisableRule,
    kAddInherit,
    kDropInherit,
    kAddOf,
    kDropOf,
    kReplicaIdentity,
    kEnableRowSecurity,
    kDisableRowSecurity,
    kForceRowSecurity,
    kNoForceRowSecurity,
    kGenericOptions,
    kAttachPartition,
    kDetachPartition,
    kDetachPartitionFinalize,
    kAddIdentity,
    kSetIdentity,
    kDropIdentity,
    kReAddStatistics,
};

enum class RTEKind {
    kRelation,
    kSubquery,
    kJoin,
    kFunction,
    kTablefunc,
    kValues,
    kCte,
    kNamedtuplestore,
    kResult,
};

enum class AExprKind {
    kOp,
    kOpAny,
    kOpAll,
    kDistinct,
    kNotDistinct,
    kNullif,
    kIn,
    kLike,
    kIlike,
    kSimilar,
    kBetween,
    kNotBetween,
    kBetweenSym,
    kNotBetweenSym,
};

enum class RoleSpecType {
    kCstring,
    kCurrentRole,
    kCurrentUser,
    kSessionUser,
    kPublic,
};

enum class DefElemAction {
    kUnspec,
    kSet,
    kAdd,
    kDrop,
};

enum class PartitionRangeDatumKind {
    kMinvalue = -1,
    kValue = 0,
    kMaxvalue = 1,
};

enum class GroupingSetKind {
    kEmpty,
    kSimple,
    kRollup,
    kCube,
    kSets,
};

enum class WCOKind {
    kViewCheck,
    kRlsInsertCheck,
    kRlsUpdateCheck,
    kRlsConflictCheck,
    kRlsMergeUpdateCheck,
    kRlsMergeDeleteCheck,
};

enum class CTEMaterialize {
    kDefault,
    kAlways,
    kNever,
};

enum class ConstrType {
    kNull,
    kNotNull,
    kDefault,
    kIdentity,
    kGenerated,
    kCheck,
    kPrimary,
    kUnique,
    kExclusion,
    kForeign,
    kAttrDeferrable,
    kAttrNotDeferrable,
    kAttrDeferred,
    kAttrImmediate,
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

class Alias;
class RangeVar;
class TypeName;
class ColumnRef;
class ParamRef;
class AExpr;
class AConst;
class TypeCast;
class CollateClause;
class RoleSpec;
class FuncCall;
class AStar;
class AIndices;
class AIndirection;
class AArrayExpr;
class ResTarget;
class MultiAssignRef;
class SortBy;
class WindowDef;
class RangeSubselect;
class RangeFunction;
class ColumnDef;
class IndexElem;
class DefElem;
class LockingClause;
class XmlSerialize;
class PartitionElem;
class PartitionSpec;
class PartitionBoundSpec;
class PartitionRangeDatum;
class PartitionCmd;
class InferClause;
class OnConflictClause;
class CommonTableExpr;
class MergeWhenClause;
class MergeAction;
class TriggerTransition;
class AccessPriv;
class IntoClause;
class WithClause;
class RawStmt;
class InsertStmt;
class DeleteStmt;
class UpdateStmt;
class SelectStmt;
class CreateStmt;
class CreateSchemaStmt;
class AlterTableStmt;
class AlterTableCmd;
class DropStmt;
class RangeTblEntry;
class Query;
class SortGroupClause;
class GroupingSet;
class WindowClause;
class RowMarkClause;
class WithCheckOption;
class RangeTblFunction;
class TableSampleClause;
// Forward declarations for additional statement node types.
class TransactionStmt;
class TruncateStmt;
class ExplainStmt;
class CommentStmt;
class IndexStmt;
class ViewStmt;
class CreateAsStmt;
class VacuumStmt;
class VariableSetStmt;
class ClusterStmt;
class LockStmt;
class DiscardStmt;
class NotifyStmt;
class ListenStmt;
class UnlistenStmt;
class CheckPointStmt;
class ReindexStmt;
class DeallocateStmt;
class PrepareStmt;
class ExecuteStmt;
class LoadStmt;
class CallStmt;
class RenameStmt;
class AlterOwnerStmt;
class CreateSeqStmt;
class AlterSeqStmt;
class CreateFunctionStmt;
class AlterFunctionStmt;
class CreateTrigStmt;
class CreateRoleStmt;
class AlterRoleStmt;
class DropRoleStmt;
class GrantStmt;
class GrantRoleStmt;
class CopyStmt;
class RefreshMatViewStmt;
class CreateTableSpaceStmt;
class DropTableSpaceStmt;
class CreatedbStmt;
class DropdbStmt;
class AlterDatabaseStmt;

// ---------------------------------------------------------------------------
// Type name and column reference nodes
// ---------------------------------------------------------------------------

// TypeName — specifies a type in definitions.
class TypeName : public Node {
public:
    TypeName() : Node(mytoydb::nodes::NodeTag::kTypeName) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> names;        // qualified name (list of String nodes)
    int type_oid = 0;                // type identified by OID
    bool setof = false;              // is a set?
    bool pct_type = false;           // %TYPE specified?
    std::vector<Node*> typmods;      // type modifier expression(s)
    int typemod = 0;                 // prespecified type modifier
    std::vector<Node*> array_bounds; // array bounds
    int location = -1;               // token location, or -1 if unknown
};

// ColumnRef — reference to a column, or possibly a whole tuple.
class ColumnRef : public Node {
public:
    ColumnRef() : Node(mytoydb::nodes::NodeTag::kColumnRef) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> fields; // field names (String nodes) or A_Star
    int location = -1;         // token location, or -1 if unknown
};

// ParamRef — specifies a $n parameter reference.
class ParamRef : public Node {
public:
    ParamRef() : Node(mytoydb::nodes::NodeTag::kParamRef) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    int number = 0;   // the number of the parameter
    int location = -1; // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// Expression nodes
// ---------------------------------------------------------------------------

// A_Expr — infix, prefix, and postfix expressions.
class AExpr : public Node {
public:
    AExpr() : Node(mytoydb::nodes::NodeTag::kAExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    AExprKind kind = AExprKind::kOp; // see above
    std::vector<Node*> name;         // possibly-qualified name of operator
    Node* lexpr = nullptr;           // left argument, or nullptr if none
    Node* rexpr = nullptr;           // right argument, or nullptr if none
    int location = -1;               // token location, or -1 if unknown
};

// A_Const — a literal constant.
class AConst : public Node {
public:
    AConst() : Node(mytoydb::nodes::NodeTag::kAConst) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    // The literal value (a Value node: Integer, Float, String, etc.).
    Node* val = nullptr;
    bool isnull = false; // SQL NULL constant
    bool isbool = false; // true if created from TRUE/FALSE keyword
    int location = -1;   // token location, or -1 if unknown
};

// TypeCast — a CAST expression.
class TypeCast : public Node {
public:
    TypeCast() : Node(mytoydb::nodes::NodeTag::kTypeCast) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* arg = nullptr;          // the expression being casted
    TypeName* type_name = nullptr; // the target type
    int location = -1;            // token location, or -1 if unknown
};

// CollateClause — a COLLATE expression.
class CollateClause : public Node {
public:
    CollateClause() : Node(mytoydb::nodes::NodeTag::kCollateClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* arg = nullptr;            // input expression
    std::vector<Node*> collname;    // possibly-qualified collation name
    int location = -1;              // token location, or -1 if unknown
};

// RoleSpec — a role name or one of a few special values.
class RoleSpec : public Node {
public:
    RoleSpec() : Node(mytoydb::nodes::NodeTag::kRoleSpec) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RoleSpecType roletype = RoleSpecType::kCstring; // type of this rolespec
    std::string rolename;  // filled only for ROLESPEC_CSTRING
    int location = -1;     // token location, or -1 if unknown
};

// FuncCall — a function or aggregate invocation.
class FuncCall : public Node {
public:
    FuncCall() : Node(mytoydb::nodes::NodeTag::kFuncCall) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> funcname;       // qualified name of function
    std::vector<Node*> args;           // the arguments (list of exprs)
    std::vector<Node*> agg_order;      // ORDER BY (list of SortBy)
    Node* agg_filter = nullptr;        // FILTER clause, if any
    WindowDef* over = nullptr;         // OVER clause, if any
    bool agg_within_group = false;     // ORDER BY appeared in WITHIN GROUP
    bool agg_star = false;             // argument was really '*'
    bool agg_distinct = false;         // arguments were labeled DISTINCT
    bool func_variadic = false;        // last argument was labeled VARIADIC
    CoercionForm funcformat = CoercionForm::kImplicit; // how to display
    int location = -1;                 // token location, or -1 if unknown
};

// A_Star — '*' representing all columns of a table or compound field.
class AStar : public Node {
public:
    AStar() : Node(mytoydb::nodes::NodeTag::kAStar) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;
};

// A_Indices — array subscript or slice bounds.
class AIndices : public Node {
public:
    AIndices() : Node(mytoydb::nodes::NodeTag::kAIndices) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool is_slice = false; // true if slice (i.e., colon present)
    Node* lidx = nullptr;  // slice lower bound, if any
    Node* uidx = nullptr;  // subscript, or slice upper bound if any
};

// A_Indirection — select a field and/or array element from an expression.
class AIndirection : public Node {
public:
    AIndirection() : Node(mytoydb::nodes::NodeTag::kAIndirection) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* arg = nullptr;             // the thing being selected from
    std::vector<Node*> indirection;  // subscripts and/or field names and/or *
};

// A_ArrayExpr — an ARRAY[] construct.
class AArrayExpr : public Node {
public:
    AArrayExpr() : Node(mytoydb::nodes::NodeTag::kAArrayExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> elements; // array element expressions
    int location = -1;           // token location, or -1 if unknown
};

// ResTarget — result target (used in target list of pre-transformed trees).
class ResTarget : public Node {
public:
    ResTarget() : Node(mytoydb::nodes::NodeTag::kResTarget) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name;               // column name or empty
    std::vector<Node*> indirection; // subscripts, field names, and '*', or NIL
    Node* val = nullptr;            // the value expression to compute or assign
    int location = -1;              // token location, or -1 if unknown
};

// MultiAssignRef — element of a row source expression for UPDATE.
class MultiAssignRef : public Node {
public:
    MultiAssignRef() : Node(mytoydb::nodes::NodeTag::kMultiAssignRef) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* source = nullptr; // the row-valued expression
    int colno = 0;          // column number for this target (1..n)
    int ncolumns = 0;       // number of targets in the construct
};

// SortBy — for ORDER BY clause.
class SortBy : public Node {
public:
    SortBy() : Node(mytoydb::nodes::NodeTag::kSortBy) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* node = nullptr;             // expression to sort on
    SortByDir sortby_dir = SortByDir::kDefault;     // ASC/DESC/USING/default
    SortByNulls sortby_nulls = SortByNulls::kDefault; // NULLS FIRST/LAST
    std::vector<Node*> use_op;        // name of op to use, if SORTBY_USING
    int location = -1;                // operator location, or -1 if none
};

// WindowDef — raw representation of WINDOW and OVER clauses.
class WindowDef : public Node {
public:
    WindowDef() : Node(mytoydb::nodes::NodeTag::kWindowDef) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name;                    // window's own name
    std::string refname;                 // referenced window name, if any
    std::vector<Node*> partition_clause; // PARTITION BY expression list
    std::vector<Node*> order_clause;     // ORDER BY (list of SortBy)
    int frame_options = 0;               // frame_clause options, see below
    Node* start_offset = nullptr;        // expression for starting bound, if any
    Node* end_offset = nullptr;          // expression for ending bound, if any
    int location = -1;                   // parse location, or -1 if none
};

// ---------------------------------------------------------------------------
// Range table node types
// ---------------------------------------------------------------------------

// RangeSubselect — subquery appearing in a FROM clause.
class RangeSubselect : public Node {
public:
    RangeSubselect() : Node(mytoydb::nodes::NodeTag::kRangeSubselect) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool lateral = false;     // does it have LATERAL prefix?
    Node* subquery = nullptr; // the untransformed sub-select clause
    Alias* alias = nullptr;   // table alias & optional column aliases
};

// RangeFunction — function call appearing in a FROM clause.
class RangeFunction : public Node {
public:
    RangeFunction() : Node(mytoydb::nodes::NodeTag::kRangeFunction) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool lateral = false;       // does it have LATERAL prefix?
    bool ordinality = false;    // does it have WITH ORDINALITY suffix?
    bool is_rowsfrom = false;   // is result of ROWS FROM() syntax?
    std::vector<Node*> functions; // per-function information, see above
    Alias* alias = nullptr;     // table alias & optional column aliases
    std::vector<Node*> coldeflist; // list of ColumnDef nodes for RECORD return
};

// ColumnDef — column definition (used in various creates).
class ColumnDef : public Node {
public:
    ColumnDef() : Node(mytoydb::nodes::NodeTag::kColumnDef) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string colname;          // name of column
    TypeName* type_name = nullptr; // type of column
    std::string compression;      // compression method for column
    int inhcount = 0;             // number of times column is inherited
    bool is_local = false;        // column has local (non-inherited) def'n
    bool is_not_null = false;     // NOT NULL constraint specified?
    bool is_from_type = false;    // column definition came from table type
    char storage = 0;             // attstorage setting, or 0 for default
    Node* raw_default = nullptr;  // default value (untransformed parse tree)
    Node* cooked_default = nullptr; // default value (transformed expr tree)
    char identity = 0;            // attidentity setting
    RangeVar* identity_sequence = nullptr; // identity sequence name
    char generated = 0;           // attgenerated setting
    CollateClause* coll_clause = nullptr; // untransformed COLLATE spec, if any
    int coll_oid = 0;             // collation OID (0 if not set)
    std::vector<Node*> constraints; // other constraints on column
    std::vector<Node*> fdwoptions; // per-column FDW options
    int location = -1;            // parse location, or -1 if none
};

// IndexElem — index parameters (used in CREATE INDEX, and in ON CONFLICT).
class IndexElem : public Node {
public:
    IndexElem() : Node(mytoydb::nodes::NodeTag::kIndexElem) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name;                  // name of attribute to index, or empty
    Node* expr = nullptr;              // expression to index, or nullptr
    std::string indexcolname;          // name for index column; empty = default
    std::vector<Node*> collation;      // name of collation; NIL = default
    std::vector<Node*> opclass;        // name of desired opclass; NIL = default
    std::vector<Node*> opclassopts;    // opclass-specific options, or NIL
    SortByDir ordering = SortByDir::kDefault;       // ASC/DESC/default
    SortByNulls nulls_ordering = SortByNulls::kDefault; // FIRST/LAST/default
};

// DefElem — a generic "name = value" option definition.
class DefElem : public Node {
public:
    DefElem() : Node(mytoydb::nodes::NodeTag::kDefElem) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string defnamespace; // empty if unqualified name
    std::string defname;
    Node* arg = nullptr;      // typically Integer, Float, String, or TypeName
    DefElemAction defaction = DefElemAction::kUnspec; // SET/ADD/DROP
    int location = -1;        // token location, or -1 if unknown
};

// LockingClause — raw representation of FOR [NO KEY] UPDATE/[KEY] SHARE.
class LockingClause : public Node {
public:
    LockingClause() : Node(mytoydb::nodes::NodeTag::kLockingClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> locked_rels;   // FOR [KEY] UPDATE/SHARE relations
    LockClauseStrength strength = LockClauseStrength::kNone;
    LockWaitPolicy wait_policy = LockWaitPolicy::kBlock; // NOWAIT and SKIP LOCKED
};

// XmlSerialize — XMLSERIALIZE (in raw parse tree only).
class XmlSerialize : public Node {
public:
    XmlSerialize() : Node(mytoydb::nodes::NodeTag::kXmlSerialize) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    XmlOptionType xmloption = XmlOptionType::kDocument; // DOCUMENT or CONTENT
    Node* expr = nullptr;
    TypeName* type_name = nullptr;
    int location = -1; // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// Partitioning node types
// ---------------------------------------------------------------------------

// PartitionElem — parse-time representation of a single partition key.
class PartitionElem : public Node {
public:
    PartitionElem() : Node(mytoydb::nodes::NodeTag::kPartitionElem) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name;              // name of column to partition on, or empty
    Node* expr = nullptr;          // expression to partition on, or nullptr
    std::vector<Node*> collation;  // name of collation; NIL = default
    std::vector<Node*> opclass;    // name of desired opclass; NIL = default
    int location = -1;             // token location, or -1 if unknown
};

// PartitionSpec — parse-time representation of a partition key specification.
class PartitionSpec : public Node {
public:
    PartitionSpec() : Node(mytoydb::nodes::NodeTag::kPartitionSpec) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string strategy;       // partitioning strategy ('hash', 'list', 'range')
    std::vector<Node*> part_params; // list of PartitionElems
    int location = -1;          // token location, or -1 if unknown
};

// PartitionBoundSpec — a partition bound specification.
class PartitionBoundSpec : public Node {
public:
    PartitionBoundSpec() : Node(mytoydb::nodes::NodeTag::kPartitionBoundSpec) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    char strategy = 0;       // 'h', 'l', or 'r'
    bool is_default = false; // is it a default partition bound?
    int modulus = 0;         // HASH strategy
    int remainder = 0;       // HASH strategy
    std::vector<Node*> listdatums;  // LIST strategy: list of Consts/A_Consts
    std::vector<Node*> lowerdatums; // RANGE strategy: list of PartitionRangeDatums
    std::vector<Node*> upperdatums; // RANGE strategy: list of PartitionRangeDatums
    int location = -1;       // token location, or -1 if unknown
};

// PartitionRangeDatum — one of the values in a range partition bound.
class PartitionRangeDatum : public Node {
public:
    PartitionRangeDatum() : Node(mytoydb::nodes::NodeTag::kPartitionRangeDatum) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    PartitionRangeDatumKind kind = PartitionRangeDatumKind::kValue;
    Node* value = nullptr; // Const (or A_Const), if kind is kValue, else nullptr
    int location = -1;     // token location, or -1 if unknown
};

// PartitionCmd — info for ALTER TABLE/INDEX ATTACH/DETACH PARTITION commands.
class PartitionCmd : public Node {
public:
    PartitionCmd() : Node(mytoydb::nodes::NodeTag::kPartitionCmd) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* name = nullptr;          // name of partition to attach/detach
    PartitionBoundSpec* bound = nullptr; // FOR VALUES, if attaching
    bool concurrent = false;
};

// ---------------------------------------------------------------------------
// Range table entry and related types (for parse analysis)
// ---------------------------------------------------------------------------

// RangeTblEntry — a range table is a list of RangeTblEntry nodes.
class RangeTblEntry : public Node {
public:
    RangeTblEntry() : Node(mytoydb::nodes::NodeTag::kRangeTblEntry) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RTEKind rtekind = RTEKind::kRelation;

    // Fields valid for a plain relation RTE:
    int relid = 0;             // OID of the relation
    char relkind = 0;          // relation kind
    int rellockmode = 0;       // lock level that query requires on the rel
    TableSampleClause* tablesample = nullptr; // sampling info, or nullptr

    // Fields valid for a subquery RTE:
    Query* subquery = nullptr; // the sub-query
    bool security_barrier = false;

    // Fields valid for a join RTE:
    JoinType jointype = JoinType::kInner;
    int joinmergedcols = 0;       // number of merged (JOIN USING) columns
    std::vector<Node*> joinaliasvars; // list of alias-var expansions
    std::vector<Node*> joinleftcols;  // left-side input column numbers
    std::vector<Node*> joinrightcols; // right-side input column numbers
    Alias* join_using_alias = nullptr; // alias clause attached to JOIN/USING

    // Fields valid for a function RTE:
    std::vector<Node*> functions; // list of RangeTblFunction nodes
    bool funcordinality = false;  // is this called WITH ORDINALITY?

    // Fields valid for a TableFunc RTE:
    Node* tablefunc = nullptr;

    // Fields valid for a values RTE:
    std::vector<Node*> values_lists; // list of expression lists

    // Fields valid for a CTE RTE:
    std::string ctename;       // name of the WITH list item
    int ctelevelsup = 0;       // number of query levels up
    bool self_reference = false; // is this a recursive self-reference?

    // Fields valid for CTE, VALUES, ENR, and TableFunc RTEs:
    std::vector<Node*> coltypes;       // OID list of column type OIDs
    std::vector<Node*> coltypmods;     // integer list of column typmods
    std::vector<Node*> colcollations;  // OID list of column collation OIDs

    // Fields valid for ENR RTEs:
    std::string enrname;  // name of ephemeral named relation
    double enrtuples = 0; // estimated or actual from caller

    // Fields valid in all RTEs:
    Alias* alias = nullptr;     // user-written alias clause, if any
    Alias* eref = nullptr;      // expanded reference names
    bool lateral = false;       // subquery, function, or values is LATERAL?
    bool inh = false;           // inheritance requested?
    bool in_from_cl = false;    // present in FROM clause?
    int required_perms = 0;     // bitmask of required access permissions
    int check_as_user = 0;      // if valid, check access as this role
    void* selected_cols = nullptr;     // columns needing SELECT (Bitmapset*)
    void* inserted_cols = nullptr;     // columns needing INSERT (Bitmapset*)
    void* updated_cols = nullptr;      // columns needing UPDATE (Bitmapset*)
    void* extra_updated_cols = nullptr; // generated columns being updated
    std::vector<Node*> security_quals; // security barrier quals to apply
};

// RangeTblFunction — subsidiary data for one function in a FUNCTION RTE.
class RangeTblFunction : public Node {
public:
    RangeTblFunction() : Node(mytoydb::nodes::NodeTag::kRangeTblFunction) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* funcexpr = nullptr;  // expression tree for func call
    int funccolcount = 0;      // number of columns it contributes to RTE
    std::vector<Node*> funccolnames;    // column names (list of String)
    std::vector<Node*> funccoltypes;    // OID list of column type OIDs
    std::vector<Node*> funccoltypmods;  // integer list of column typmods
    std::vector<Node*> funccolcollations; // OID list of column collation OIDs
    void* funcparams = nullptr; // PARAM_EXEC Param IDs (Bitmapset*)
};

// TableSampleClause — TABLESAMPLE appearing in a transformed FROM clause.
class TableSampleClause : public Node {
public:
    TableSampleClause() : Node(mytoydb::nodes::NodeTag::kTableSampleClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    int tsmhandler = 0;       // OID of the tablesample handler function
    std::vector<Node*> args;  // tablesample argument expression(s)
    Node* repeatable = nullptr; // REPEATABLE expression, or nullptr if none
};

// WithCheckOption — WITH CHECK OPTION checks.
class WithCheckOption : public Node {
public:
    WithCheckOption() : Node(mytoydb::nodes::NodeTag::kWithCheckOption) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    WCOKind kind = WCOKind::kViewCheck; // kind of WCO
    std::string relname;  // name of relation that specified the WCO
    std::string polname;  // name of RLS policy being checked
    Node* qual = nullptr; // constraint qual to check
    bool cascaded = false; // true for a cascaded WCO on a view
};

// ---------------------------------------------------------------------------
// Sort/group/window clause types
// ---------------------------------------------------------------------------

// SortGroupClause — representation of ORDER BY, GROUP BY, etc. items.
class SortGroupClause : public Node {
public:
    SortGroupClause() : Node(mytoydb::nodes::NodeTag::kSortGroupClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    int tle_sort_group_ref = 0; // reference into targetlist
    int eqop = 0;               // the equality operator ('=' op)
    int sortop = 0;             // the ordering operator ('<' op), or 0
    bool nulls_first = false;   // do NULLs come before normal values?
    bool hashable = false;      // can eqop be implemented by hashing?
};

// GroupingSet — representation of CUBE, ROLLUP and GROUPING SETS clauses.
class GroupingSet : public Node {
public:
    GroupingSet() : Node(mytoydb::nodes::NodeTag::kGroupingSet) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    GroupingSetKind kind = GroupingSetKind::kEmpty;
    std::vector<Node*> content;
    int location = -1;
};

// WindowClause — transformed representation of WINDOW and OVER clauses.
class WindowClause : public Node {
public:
    WindowClause() : Node(mytoydb::nodes::NodeTag::kWindowClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name;                     // window name (empty in an OVER clause)
    std::string refname;                  // referenced window name, if any
    std::vector<Node*> partition_clause;  // PARTITION BY list
    std::vector<Node*> order_clause;      // ORDER BY list
    int frame_options = 0;                // frame_clause options
    Node* start_offset = nullptr;         // expression for starting bound
    Node* end_offset = nullptr;           // expression for ending bound
    std::vector<Node*> run_condition;     // qual to help short-circuit execution
    int start_in_range_func = 0;          // in_range function for startOffset
    int end_in_range_func = 0;            // in_range function for endOffset
    int in_range_coll = 0;                // collation for in_range tests
    bool in_range_asc = false;            // use ASC sort order for in_range?
    bool in_range_nulls_first = false;    // nulls sort first for in_range?
    int winref = 0;                       // ID referenced by window functions
    bool copied_order = false;            // did we copy orderClause from refname?
};

// RowMarkClause — parser output representation of FOR [KEY] UPDATE/SHARE.
class RowMarkClause : public Node {
public:
    RowMarkClause() : Node(mytoydb::nodes::NodeTag::kRowMarkClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    int rti = 0;           // range table index of target relation
    LockClauseStrength strength = LockClauseStrength::kNone;
    LockWaitPolicy wait_policy = LockWaitPolicy::kBlock;
    bool pushed_down = false; // pushed down from higher query level?
};

// ---------------------------------------------------------------------------
// WITH clause and conflict clause types
// ---------------------------------------------------------------------------

// WithClause — representation of WITH clause.
class WithClause : public Node {
public:
    WithClause() : Node(mytoydb::nodes::NodeTag::kWithClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> ctes; // list of CommonTableExprs
    bool recursive = false;  // true = WITH RECURSIVE
    int location = -1;       // token location, or -1 if unknown
};

// InferClause — ON CONFLICT unique index inference clause.
class InferClause : public Node {
public:
    InferClause() : Node(mytoydb::nodes::NodeTag::kInferClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> index_elems; // IndexElems to infer unique index
    Node* where_clause = nullptr;   // qualification (partial-index predicate)
    std::string conname;            // Constraint name, or empty if unnamed
    int location = -1;              // token location, or -1 if unknown
};

// OnConflictClause — representation of ON CONFLICT clause.
class OnConflictClause : public Node {
public:
    OnConflictClause() : Node(mytoydb::nodes::NodeTag::kOnConflictClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    OnConflictAction action = OnConflictAction::kNone; // DO NOTHING or UPDATE?
    InferClause* infer = nullptr;  // Optional index inference clause
    std::vector<Node*> target_list; // the target list (of ResTarget)
    Node* where_clause = nullptr;  // qualifications
    int location = -1;             // token location, or -1 if unknown
};

// CommonTableExpr — representation of WITH list element.
class CommonTableExpr : public Node {
public:
    CommonTableExpr() : Node(mytoydb::nodes::NodeTag::kCommonTableExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string ctename;               // query name (never qualified)
    std::vector<Node*> aliascolnames;  // optional list of column names
    CTEMaterialize ctematerialized = CTEMaterialize::kDefault; // optimization fence?
    Node* ctequery = nullptr;          // the CTE's subquery
    Node* search_clause = nullptr;     // CTESearchClause (as Node* for now)
    Node* cycle_clause = nullptr;      // CTECycleClause (as Node* for now)
    int location = -1;                 // token location, or -1 if unknown
    // Fields set during parse analysis:
    bool cterecursive = false;
    int cterefcount = 0;
    std::vector<Node*> ctecolnames;      // list of output column names
    std::vector<Node*> ctecoltypes;      // OID list of output column type OIDs
    std::vector<Node*> ctecoltypmods;    // integer list of output column typmods
    std::vector<Node*> ctecolcollations; // OID list of column collation OIDs
};

// ---------------------------------------------------------------------------
// MERGE statement types
// ---------------------------------------------------------------------------

// MergeWhenClause — raw parser representation of a WHEN clause in MERGE.
class MergeWhenClause : public Node {
public:
    MergeWhenClause() : Node(mytoydb::nodes::NodeTag::kMergeWhenClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool matched = false;          // true=MATCHED, false=NOT MATCHED
    CmdType command_type = CmdType::kNothing; // INSERT/UPDATE/DELETE/DO NOTHING
    OverridingKind override_kind = OverridingKind::kNotSet; // OVERRIDING clause
    Node* condition = nullptr;     // WHEN conditions (raw parser)
    std::vector<Node*> target_list; // INSERT/UPDATE targetlist
    std::vector<Node*> values;     // VALUES to INSERT, or empty
};

// MergeAction — transformed representation of a WHEN clause in MERGE.
class MergeAction : public Node {
public:
    MergeAction() : Node(mytoydb::nodes::NodeTag::kMergeAction) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool matched = false;          // true=MATCHED, false=NOT MATCHED
    CmdType command_type = CmdType::kNothing; // INSERT/UPDATE/DELETE/DO NOTHING
    OverridingKind override_kind = OverridingKind::kNotSet; // OVERRIDING clause
    Node* qual = nullptr;          // transformed WHEN conditions
    std::vector<Node*> target_list; // the target list (of TargetEntry)
    std::vector<Node*> update_colnos; // target attribute numbers of UPDATE
};

// TriggerTransition — representation of transition row or table naming clause.
class TriggerTransition : public Node {
public:
    TriggerTransition() : Node(mytoydb::nodes::NodeTag::kTriggerTransition) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name;
    bool is_new = false;
    bool is_table = false;
};

// ---------------------------------------------------------------------------
// Privilege and role types
// ---------------------------------------------------------------------------

// AccessPriv — an access privilege, with optional list of column names.
class AccessPriv : public Node {
public:
    AccessPriv() : Node(mytoydb::nodes::NodeTag::kAccessPriv) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string priv_name;     // string name of privilege, empty = ALL
    std::vector<Node*> cols;   // list of String
};

// ---------------------------------------------------------------------------
// Alias and range var types
// ---------------------------------------------------------------------------

// Alias — specifies an alias for a range variable.
class Alias : public Node {
public:
    Alias() : Node(mytoydb::nodes::NodeTag::kAlias) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string aliasname;          // aliased rel name (never qualified)
    std::vector<Node*> colnames;    // optional list of column aliases
};

// RangeVar — range variable, used in FROM clauses.
class RangeVar : public Node {
public:
    RangeVar() : Node(mytoydb::nodes::NodeTag::kRangeVar) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string catalogname;  // the catalog (database) name, or empty
    std::string schemaname;   // the schema name, or empty
    std::string relname;      // the relation/sequence name
    bool inh = false;         // expand rel by inheritance? recursively?
    char relpersistence = 0;  // see RELPERSISTENCE_* in pg_class.h
    Alias* alias = nullptr;   // table alias & optional column aliases
    int location = -1;        // token location, or -1 if unknown
};

// IntoClause — target information for SELECT INTO, CREATE TABLE AS, etc.
class IntoClause : public Node {
public:
    IntoClause() : Node(mytoydb::nodes::NodeTag::kIntoClause) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* rel = nullptr;          // target relation name
    std::vector<Node*> col_names;     // column names to assign, or NIL
    std::string access_method;        // table access method
    std::vector<Node*> options;       // options from WITH clause
    OnCommitAction on_commit = OnCommitAction::kNoop; // what do at COMMIT?
    std::string table_space_name;     // table space to use, or empty
    Node* view_query = nullptr;       // materialized view's SELECT query
    bool skip_data = false;           // true for WITH NO DATA
};

// ---------------------------------------------------------------------------
// Statement nodes
// ---------------------------------------------------------------------------

// RawStmt — container for any one statement's raw parse tree.
class RawStmt : public Node {
public:
    RawStmt() : Node(mytoydb::nodes::NodeTag::kRawStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* stmt = nullptr;     // raw parse tree
    int stmt_location = -1;   // start location, or -1 if unknown
    int stmt_len = 0;         // length in bytes; 0 means "rest of string"
};

// InsertStmt — INSERT statement.
class InsertStmt : public Node {
public:
    InsertStmt() : Node(mytoydb::nodes::NodeTag::kInsertStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* relation = nullptr;     // relation to insert into
    std::vector<Node*> cols;          // optional: names of the target columns
    Node* select_stmt = nullptr;      // the source SELECT/VALUES, or nullptr
    OnConflictClause* on_conflict_clause = nullptr; // ON CONFLICT clause
    std::vector<Node*> returning_list; // list of expressions to return
    WithClause* with_clause = nullptr; // WITH clause
    OverridingKind override_kind = OverridingKind::kNotSet; // OVERRIDING clause
};

// DeleteStmt — DELETE statement.
class DeleteStmt : public Node {
public:
    DeleteStmt() : Node(mytoydb::nodes::NodeTag::kDeleteStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* relation = nullptr;      // relation to delete from
    std::vector<Node*> using_clause;   // optional using clause for more tables
    Node* where_clause = nullptr;      // qualifications
    std::vector<Node*> returning_list; // list of expressions to return
    WithClause* with_clause = nullptr; // WITH clause
};

// UpdateStmt — UPDATE statement.
class UpdateStmt : public Node {
public:
    UpdateStmt() : Node(mytoydb::nodes::NodeTag::kUpdateStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* relation = nullptr;      // relation to update
    std::vector<Node*> target_list;    // the target list (of ResTarget)
    Node* where_clause = nullptr;      // qualifications
    std::vector<Node*> from_clause;    // optional from clause for more tables
    std::vector<Node*> returning_list; // list of expressions to return
    WithClause* with_clause = nullptr; // WITH clause
};

// SelectStmt — SELECT statement (also used for VALUES and set operations).
class SelectStmt : public Node {
public:
    SelectStmt() : Node(mytoydb::nodes::NodeTag::kSelectStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    // Fields used only in "leaf" SelectStmts:
    std::vector<Node*> distinct_clause; // NULL, list of DISTINCT ON exprs, etc.
    IntoClause* into_clause = nullptr;  // target for SELECT INTO
    std::vector<Node*> target_list;     // the target list (of ResTarget)
    std::vector<Node*> from_clause;     // the FROM clause
    Node* where_clause = nullptr;       // WHERE qualification
    std::vector<Node*> group_clause;    // GROUP BY clauses
    bool group_distinct = false;        // Is this GROUP BY DISTINCT?
    Node* having_clause = nullptr;      // HAVING conditional-expression
    std::vector<Node*> window_clause;   // WINDOW window_name AS (...), ...

    // In a "leaf" node representing a VALUES list:
    std::vector<std::vector<Node*>> values_lists; // untransformed list of expression lists

    // Fields used in both "leaf" and upper-level SelectStmts:
    std::vector<Node*> sort_clause;   // sort clause (a list of SortBy's)
    Node* limit_offset = nullptr;     // # of result tuples to skip
    Node* limit_count = nullptr;      // # of result tuples to return
    LimitOption limit_option = LimitOption::kCount; // limit type
    std::vector<Node*> locking_clause; // FOR UPDATE (list of LockingClause's)
    WithClause* with_clause = nullptr; // WITH clause

    // Fields used only in upper-level SelectStmts:
    SetOperation op = SetOperation::kNone; // type of set op
    bool all = false;                  // ALL specified?
    SelectStmt* larg = nullptr;        // left child
    SelectStmt* rarg = nullptr;        // right child
};

// CreateStmt — CREATE TABLE statement.
class CreateStmt : public Node {
public:
    CreateStmt() : Node(mytoydb::nodes::NodeTag::kCreateStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* relation = nullptr;          // relation to create
    std::vector<Node*> table_elts;        // column definitions (list of ColumnDef)
    std::vector<Node*> inh_relations;     // relations to inherit from (RangeVar)
    PartitionBoundSpec* partbound = nullptr; // FOR VALUES clause
    PartitionSpec* partspec = nullptr;    // PARTITION BY clause
    TypeName* of_typename = nullptr;      // OF typename
    std::vector<Node*> constraints;       // constraints (list of Constraint)
    std::vector<Node*> options;           // options from WITH clause
    OnCommitAction oncommit = OnCommitAction::kNoop; // what do at COMMIT?
    std::string tablespacename;           // table space to use, or empty
    std::string access_method;            // table access method
    bool if_not_exists = false;           // just do nothing if it already exists?
};

// CreateSchemaStmt — CREATE SCHEMA statement.
class CreateSchemaStmt : public Node {
public:
    CreateSchemaStmt() : Node(mytoydb::nodes::NodeTag::kCreateSchemaStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string schemaname;       // the name of the schema to create
    RoleSpec* authrole = nullptr; // the owner of the created schema
    std::vector<Node*> schema_elts; // schema components (list of parsenodes)
    bool if_not_exists = false;   // just do nothing if schema already exists?
};

// AlterTableStmt — ALTER TABLE statement.
class AlterTableStmt : public Node {
public:
    AlterTableStmt() : Node(mytoydb::nodes::NodeTag::kAlterTableStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* relation = nullptr; // table to work on
    std::vector<Node*> cmds;      // list of subcommands
    ObjectType objtype = ObjectType::kTable; // type of object
    bool missing_ok = false;      // skip error if table missing
};

// AlterTableCmd — one subcommand of an ALTER TABLE.
class AlterTableCmd : public Node {
public:
    AlterTableCmd() : Node(mytoydb::nodes::NodeTag::kAlterTableCmd) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    AlterTableType subtype = AlterTableType::kAddColumn; // type of alteration
    std::string name;          // column, constraint, or trigger to act on
    std::string newname;       // new name for RENAME actions
    int num = 0;               // attribute number for columns referenced by number
    RoleSpec* newowner = nullptr; // new owner
    Node* def = nullptr;       // definition of new column, index, constraint
    DropBehavior behavior = DropBehavior::kRestrict; // RESTRICT or CASCADE
    bool missing_ok = false;   // skip error if missing?
    bool recurse = false;      // exec-time recursion
};

// DropStmt — DROP statement (Table|Sequence|View|Index|Type|Domain|...).
class DropStmt : public Node {
public:
    DropStmt() : Node(mytoydb::nodes::NodeTag::kDropStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> objects; // list of names
    ObjectType remove_type = ObjectType::kTable; // object type
    DropBehavior behavior = DropBehavior::kRestrict; // RESTRICT or CASCADE
    bool missing_ok = false;    // skip error if object is missing?
    bool concurrent = false;    // drop index concurrently?
};

// ---------------------------------------------------------------------------
// Additional statement node types (Phase 5 grammar expansion)
// ---------------------------------------------------------------------------

// TransactionStmt — BEGIN / COMMIT / ROLLBACK / SAVEPOINT / etc.
class TransactionStmt : public Node {
public:
    TransactionStmt() : Node(mytoydb::nodes::NodeTag::kTransactionStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    enum class Kind {
        kBegin,
        kStart,
        kCommit,
        kRollback,
        kSavepoint,
        kRelease,
        kRollbackTo,
        kPrepare,
        kCommitPrepared,
        kRollbackPrepared,
    };

    Kind kind = Kind::kBegin;
    std::string savepoint_name; // for SAVEPOINT / RELEASE / ROLLBACK TO
    std::vector<Node*> options; // transaction options (list of DefElem)
    std::string gid;            // for PREPARE/COMMIT PREPARED
};

// TruncateStmt — TRUNCATE TABLE
class TruncateStmt : public Node {
public:
    TruncateStmt() : Node(mytoydb::nodes::NodeTag::kTruncateStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> relations; // list of RangeVar
    bool restart_seqs = false;    // restart sequences?
    DropBehavior behavior = DropBehavior::kRestrict;
};

// ExplainStmt — EXPLAIN
class ExplainStmt : public Node {
public:
    ExplainStmt() : Node(mytoydb::nodes::NodeTag::kExplainStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* query = nullptr;        // the query (or utility statement) to explain
    std::vector<Node*> options;   // list of DefElem
};

// CommentStmt — COMMENT ON
class CommentStmt : public Node {
public:
    CommentStmt() : Node(mytoydb::nodes::NodeTag::kCommentStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    ObjectType objtype = ObjectType::kTable;
    std::vector<Node*> object; // qualified name
    std::string comment;       // comment text (empty string means drop)
};

// IndexStmt — CREATE INDEX
class IndexStmt : public Node {
public:
    IndexStmt() : Node(mytoydb::nodes::NodeTag::kIndexStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string idxname;            // name of new index, or NULL for default
    RangeVar* relation = nullptr;   // relation to build index on
    std::string access_method;      // name of access method (e.g. btree)
    std::vector<Node*> index_params; // list of IndexElem
    std::vector<Node*> index_including_params; // list of IndexElem for INCLUDE
    std::vector<Node*> options;     // WITH clause options
    std::vector<Node*> where_clause; // WHERE clause for partial index
    bool unique = false;            // is UNIQUE?
    bool primary = false;           // is PRIMARY KEY index?
    bool concurrent = false;        // is CONCURRENTLY?
    bool if_not_exists = false;     // just do nothing if index already exists?
    bool reset_default_tblspc = true;
};

// ViewStmt — CREATE VIEW
class ViewStmt : public Node {
public:
    ViewStmt() : Node(mytoydb::nodes::NodeTag::kViewStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* view = nullptr;     // view relation
    std::vector<Node*> aliases;   // column names (list of String)
    Node* query = nullptr;        // the SELECT query
    bool replace = false;         // OR REPLACE?
    std::vector<Node*> options;   // WITH clause options
    bool with_check_option = false; // WITH CHECK OPTION?
};

// CreateAsStmt — CREATE TABLE AS SELECT / CREATE MATERIALIZED VIEW
class CreateAsStmt : public Node {
public:
    CreateAsStmt() : Node(mytoydb::nodes::NodeTag::kCreateAsStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    IntoClause* into = nullptr;  // target table spec
    Node* query = nullptr;       // source query
    bool is_select_into = false; // SELECT INTO?
    bool if_not_exists = false;  // IF NOT EXISTS?
};

// VacuumStmt — VACUUM / ANALYZE
class VacuumStmt : public Node {
public:
    VacuumStmt() : Node(mytoydb::nodes::NodeTag::kVacuumStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> options;  // list of DefElem
    std::vector<Node*> rels;     // list of VacuumRelation (empty = all)
    bool is_vacuumcmd = false;   // false for ANALYZE
};

// VariableSetStmt — SET / RESET
class VariableSetStmt : public Node {
public:
    VariableSetStmt() : Node(mytoydb::nodes::NodeTag::kVariableSetStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    enum class Kind {
        kSet,
        kLocal,
        kSession,
        kReset,
        kResetAll,
        kCurrent,
    };

    Kind kind = Kind::kSet;
    std::string name;          // variable to set
    std::vector<Node*> args;   // arguments (list of AConst or ColumnRef)
    bool is_local = false;     // SET LOCAL?
};

// ClusterStmt — CLUSTER
class ClusterStmt : public Node {
public:
    ClusterStmt() : Node(mytoydb::nodes::NodeTag::kClusterStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* relation = nullptr; // relation to cluster
    std::string indexname;        // index to cluster on, or empty for default
    bool verbose = false;
};

// LockStmt — LOCK TABLE
class LockStmt : public Node {
public:
    LockStmt() : Node(mytoydb::nodes::NodeTag::kLockStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> relations; // list of RangeVar
    int mode = 0;                 // lock mode
    bool nowait = false;          // NOWAIT?
};

// DiscardStmt — DISCARD
class DiscardStmt : public Node {
public:
    DiscardStmt() : Node(mytoydb::nodes::NodeTag::kDiscardStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    enum class Target { kAll, kSequences, kTemp };
    Target target = Target::kAll;
};

// NotifyStmt / ListenStmt / UnlistenStmt — LISTEN / NOTIFY / UNLISTEN
class NotifyStmt : public Node {
public:
    NotifyStmt() : Node(mytoydb::nodes::NodeTag::kNotifyStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string conditionname; // channel name
    std::string payload;       // payload string
};

class ListenStmt : public Node {
public:
    ListenStmt() : Node(mytoydb::nodes::NodeTag::kListenStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string conditionname; // channel name
};

class UnlistenStmt : public Node {
public:
    UnlistenStmt() : Node(mytoydb::nodes::NodeTag::kUnlistenStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string conditionname; // channel name (empty = UNLISTEN *)
};

// CheckPointStmt — CHECKPOINT
class CheckPointStmt : public Node {
public:
    CheckPointStmt() : Node(mytoydb::nodes::NodeTag::kCheckPointStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;
};

// ReindexStmt — REINDEX
class ReindexStmt : public Node {
public:
    ReindexStmt() : Node(mytoydb::nodes::NodeTag::kReindexStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    enum class Kind { kIndex, kTable, kSchema, kSystem, kDatabase };
    Kind kind = Kind::kIndex;
    std::string name;        // name of object
    std::vector<Node*> options; // list of DefElem
    bool concurrently = false;
};

// DeallocateStmt — DEALLOCATE
class DeallocateStmt : public Node {
public:
    DeallocateStmt() : Node(mytoydb::nodes::NodeTag::kDeallocateStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name; // name of prepared statement (empty = DEALLOCATE ALL)
};

// PrepareStmt — PREPARE
class PrepareStmt : public Node {
public:
    PrepareStmt() : Node(mytoydb::nodes::NodeTag::kPrepareStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name;          // name of prepared statement
    std::vector<Node*> argtypes; // list of TypeName
    Node* query = nullptr;     // the query
};

// ExecuteStmt — EXECUTE
class ExecuteStmt : public Node {
public:
    ExecuteStmt() : Node(mytoydb::nodes::NodeTag::kExecuteStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string name;          // name of prepared statement
    std::vector<Node*> params; // parameter values
};

// LoadStmt — LOAD
class LoadStmt : public Node {
public:
    LoadStmt() : Node(mytoydb::nodes::NodeTag::kLoadStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string filename; // file to load
};

// CallStmt — CALL (procedure call)
class CallStmt : public Node {
public:
    CallStmt() : Node(mytoydb::nodes::NodeTag::kCallStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* funccall = nullptr; // FuncCall node
};

// RenameStmt — ALTER ... RENAME TO
class RenameStmt : public Node {
public:
    RenameStmt() : Node(mytoydb::nodes::NodeTag::kRenameStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    ObjectType rename_type = ObjectType::kTable;
    ObjectType relation_type = ObjectType::kTable;
    RangeVar* relation = nullptr; // for relation renames
    std::vector<Node*> object;    // for other object types
    std::string subname;          // old name of attribute being renamed
    std::string newname;          // new name
    DropBehavior behavior = DropBehavior::kRestrict;
    bool missing_ok = false;
};

// AlterOwnerStmt — ALTER ... OWNER TO
class AlterOwnerStmt : public Node {
public:
    AlterOwnerStmt() : Node(mytoydb::nodes::NodeTag::kAlterOwnerStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    ObjectType object_type = ObjectType::kTable;
    std::vector<Node*> object; // object identity
    RoleSpec* newowner = nullptr;
};

// CreateSeqStmt — CREATE SEQUENCE
class CreateSeqStmt : public Node {
public:
    CreateSeqStmt() : Node(mytoydb::nodes::NodeTag::kCreateSeqStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* sequence = nullptr; // sequence name
    std::vector<Node*> options;   // list of DefElem
    bool for_identity = false;    // internal: for identity column?
    bool if_not_exists = false;
};

// AlterSeqStmt — ALTER SEQUENCE
class AlterSeqStmt : public Node {
public:
    AlterSeqStmt() : Node(mytoydb::nodes::NodeTag::kAlterSeqStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* sequence = nullptr;
    std::vector<Node*> options; // list of DefElem
    bool for_identity = false;
    bool missing_ok = false;
};

// CreateFunctionStmt — CREATE FUNCTION
class CreateFunctionStmt : public Node {
public:
    CreateFunctionStmt() : Node(mytoydb::nodes::NodeTag::kCreateFunctionStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool is_procedure = false;       // true if CREATE PROCEDURE
    bool replace = false;            // OR REPLACE?
    std::vector<Node*> funcname;     // qualified function name
    std::vector<Node*> parameters;   // list of FunctionParameter
    TypeName* return_type = nullptr; // return type
    std::vector<Node*> options;      // list of DefElem (AS, LANGUAGE, etc.)
};

// AlterFunctionStmt — ALTER FUNCTION
class AlterFunctionStmt : public Node {
public:
    AlterFunctionStmt() : Node(mytoydb::nodes::NodeTag::kAlterFunctionStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> funcname; // qualified function name
    std::vector<Node*> args;     // function arguments (for overload resolution)
    std::vector<Node*> actions;  // list of DefElem
};

// DropFunctionStmt — DROP FUNCTION (uses DropStmt, but kept for clarity)
// Note: PostgreSQL uses DropStmt for DROP FUNCTION. No separate node.

// CreateTrigStmt — CREATE TRIGGER
class CreateTrigStmt : public Node {
public:
    CreateTrigStmt() : Node(mytoydb::nodes::NodeTag::kCreateTrigStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool replace = false;
    bool isconstraint = false;
    std::string trigname;
    RangeVar* relation = nullptr;
    std::vector<Node*> funcname;     // qualified function name
    std::vector<Node*> args;         // list of argument expressions
    bool row = false;                // ROW trigger?
    int timing = 0;                  // BEFORE=0, INSTEAD=1, AFTER=2
    int events = 0;                  // INSERT/UPDATE/DELETE bitmask
    std::vector<Node*> columns;      // list of column names for UPDATE
    Node* when_clause = nullptr;     // WHEN condition
    std::vector<Node*> transition_rels; // list of TriggerTransition
    bool deferrable = false;
    bool initdeferred = false;
    std::string constrrel;           // constraint relation name
};

// CreateRoleStmt — CREATE ROLE / USER / GROUP
class CreateRoleStmt : public Node {
public:
    CreateRoleStmt() : Node(mytoydb::nodes::NodeTag::kCreateRoleStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    enum class Kind { kRole, kUser, kGroup };
    Kind stmt_type = Kind::kRole;
    std::string role;            // role name
    std::vector<Node*> options;  // list of DefElem
};

// AlterRoleStmt — ALTER ROLE
class AlterRoleStmt : public Node {
public:
    AlterRoleStmt() : Node(mytoydb::nodes::NodeTag::kAlterRoleStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RoleSpec* role = nullptr;
    std::vector<Node*> options; // list of DefElem
    int action = 0;             // 1 = add, -1 = drop, 0 = set
};

// DropRoleStmt — DROP ROLE
class DropRoleStmt : public Node {
public:
    DropRoleStmt() : Node(mytoydb::nodes::NodeTag::kDropRoleStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> roles;   // list of RoleSpec
    bool missing_ok = false;
};

// GrantStmt — GRANT / REVOKE
class GrantStmt : public Node {
public:
    GrantStmt() : Node(mytoydb::nodes::NodeTag::kGrantStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool is_grant = true;        // true = GRANT, false = REVOKE
    ObjectType targtype = ObjectType::kTable;
    ObjectType objtype = ObjectType::kTable;
    std::vector<Node*> privileges; // list of AccessPriv
    std::vector<Node*> targobjs;   // list of RangeVar
    bool grant_option = false;
    std::vector<Node*> grantees;   // list of RoleSpec
    DropBehavior behavior = DropBehavior::kRestrict;
};

// GrantRoleStmt — GRANT ROLE
class GrantRoleStmt : public Node {
public:
    GrantRoleStmt() : Node(mytoydb::nodes::NodeTag::kGrantRoleStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> granted_roles; // list of AccessPriv
    std::vector<Node*> grantee_roles; // list of RoleSpec
    bool is_grant = true;
    bool admin_opt = false;
    RoleSpec* grantor = nullptr;
    DropBehavior behavior = DropBehavior::kRestrict;
};

// CopyStmt — COPY
class CopyStmt : public Node {
public:
    CopyStmt() : Node(mytoydb::nodes::NodeTag::kCopyStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    RangeVar* relation = nullptr; // relation to copy
    std::vector<Node*> attlist;   // list of column names
    bool is_from = false;         // COPY FROM? (false = COPY TO)
    bool is_program = false;      // COPY FROM/TO PROGRAM?
    std::string filename;         // filename, or empty for STDIN/STDOUT
    std::vector<Node*> options;   // list of DefElem
    Node* query = nullptr;        // query (for COPY ... FROM query)
};

// RefreshMatViewStmt — REFRESH MATERIALIZED VIEW
class RefreshMatViewStmt : public Node {
public:
    RefreshMatViewStmt() : Node(mytoydb::nodes::NodeTag::kRefreshMatViewStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    bool concurrent = false;
    bool skip_data = false;
    RangeVar* relation = nullptr;
};

// CreateTableSpaceStmt — CREATE TABLESPACE
class CreateTableSpaceStmt : public Node {
public:
    CreateTableSpaceStmt() : Node(mytoydb::nodes::NodeTag::kCreateTableSpaceStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string tablespacename;
    RoleSpec* owner = nullptr;
    std::string location;
    std::vector<Node*> options;
};

// DropTableSpaceStmt — DROP TABLESPACE
class DropTableSpaceStmt : public Node {
public:
    DropTableSpaceStmt() : Node(mytoydb::nodes::NodeTag::kDropTableSpaceStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string tablespacename;
    bool missing_ok = false;
};

// CreatedbStmt — CREATE DATABASE
class CreatedbStmt : public Node {
public:
    CreatedbStmt() : Node(mytoydb::nodes::NodeTag::kCreatedbStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string dbname;
    std::vector<Node*> options; // list of DefElem
};

// DropdbStmt — DROP DATABASE
class DropdbStmt : public Node {
public:
    DropdbStmt() : Node(mytoydb::nodes::NodeTag::kDropdbStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string dbname;
    bool missing_ok = false;
    std::vector<Node*> options;
};

// AlterDatabaseStmt — ALTER DATABASE
class AlterDatabaseStmt : public Node {
public:
    AlterDatabaseStmt() : Node(mytoydb::nodes::NodeTag::kAlterDatabaseStmt) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::string dbname;
    std::vector<Node*> options; // list of DefElem
};

// ---------------------------------------------------------------------------
// Query tree (for parse analysis)
// ---------------------------------------------------------------------------

// Query — parse analysis turns all statements into a Query tree.
class Query : public Node {
public:
    Query() : Node(mytoydb::nodes::NodeTag::kQuery) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    CmdType command_type = CmdType::kSelect; // select|insert|update|delete|...
    QuerySource query_source = QuerySource::kOriginal; // where did I come from?
    int64_t query_id = 0;        // query identifier (can be set by plugins)
    bool can_set_tag = false;    // do I set the command result tag?
    Node* utility_stmt = nullptr; // non-null if commandType == CMD_UTILITY
    int result_relation = 0;     // rtable index of target relation; 0 for SELECT
    bool has_aggs = false;       // has aggregates in tlist or havingQual
    bool has_window_funcs = false; // has window functions in tlist
    bool has_target_srfs = false;  // has set-returning functions in tlist
    bool has_sub_links = false;   // has subquery SubLink
    bool has_distinct_on = false; // distinctClause is from DISTINCT ON
    bool has_recursive = false;   // WITH RECURSIVE was specified
    bool has_modifying_cte = false; // has INSERT/UPDATE/DELETE in WITH
    bool has_for_update = false;  // FOR [KEY] UPDATE/SHARE was specified
    bool has_row_security = false; // rewriter has applied some RLS policy
    bool is_return = false;       // is a RETURN statement
    std::vector<Node*> cte_list;  // WITH list (of CommonTableExpr's)
    std::vector<Node*> rtable;    // list of range table entries
    Node* jointree = nullptr;     // table join tree (FromExpr* as Node*)
    std::vector<Node*> merge_action_list; // list of actions for MERGE (only)
    bool merge_use_outer_join = false; // whether to use outer join
    std::vector<Node*> target_list; // target list (of TargetEntry)
    OverridingKind override_kind = OverridingKind::kNotSet; // OVERRIDING clause
    Node* on_conflict = nullptr;  // ON CONFLICT DO [NOTHING | UPDATE]
    std::vector<Node*> returning_list; // return-values list (of TargetEntry)
    std::vector<Node*> group_clause;   // a list of SortGroupClause's
    bool group_distinct = false;  // is the group by clause distinct?
    std::vector<Node*> grouping_sets; // a list of GroupingSet's if present
    Node* having_qual = nullptr;  // qualifications applied to groups
    std::vector<Node*> window_clause; // a list of WindowClause's
    std::vector<Node*> distinct_clause; // a list of SortGroupClause's
    std::vector<Node*> sort_clause;  // a list of SortGroupClause's
    Node* limit_offset = nullptr; // # of result tuples to skip (int8 expr)
    Node* limit_count = nullptr;  // # of result tuples to return (int8 expr)
    LimitOption limit_option = LimitOption::kCount; // limit type
    std::vector<Node*> row_marks; // a list of RowMarkClause's
    Node* set_operations = nullptr; // set-operation tree if top level of UNION/...
    std::vector<Node*> constraint_deps; // list of pg_constraint OIDs
    std::vector<Node*> with_check_options; // a list of WithCheckOption's
    int stmt_location = -1; // start location, or -1 if unknown
    int stmt_len = 0;       // length in bytes; 0 means "rest of string"
};

// ---------------------------------------------------------------------------
// Helper structs (not Node-derived; group related Query fields)
// ---------------------------------------------------------------------------

// SelectLimit — groups limit-related fields for convenience.
struct SelectLimit {
    Node* limit_offset = nullptr;
    Node* limit_count = nullptr;
    LimitOption limit_option = LimitOption::kCount;
};

// GroupClause — groups group-by-related fields for convenience.
struct GroupClause {
    std::vector<Node*> group_clause;
    bool group_distinct = false;
};

}  // namespace mytoydb::parser
