// gram.yy — Bison C++ grammar for pgcpp (converted from PostgreSQL 15).
//
// This is a C++20 conversion of PostgreSQL's src/backend/parser/gram.y.
// It uses the lalr1.cc skeleton with type-safe variant semantic values.
// Only SELECT, INSERT, UPDATE, DELETE, CREATE TABLE, and DROP TABLE are
// fully implemented; other statement types are stubbed out.

%require "3.8"
%skeleton "lalr1.cc"
%defines
%define api.namespace {pgcpp_parser}
%define api.parser.class {BisonParser}
%define api.value.type variant
%define api.token.constructor
%define api.location.type {int}
%define parse.error verbose
%define parse.trace
%locations

%code requires {
#include <string>
#include <vector>
#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "parser/parser_driver.hpp"

using namespace pgcpp::nodes;
using namespace pgcpp::parser;
using pgcpp::memory::palloc;

// YYLLOC_DEFAULT for int location type: just take the first location.
#ifndef YYLLOC_DEFAULT
#define YYLLOC_DEFAULT(Current, Rhs, N) \
    do { (Current) = (N) ? ((Rhs)[1].location) : ((Rhs)[0].location); } while (0)
#endif
}

%param { ParserDriver& driver }

%code {
// Helper functions — C++20 conversions of PostgreSQL's makeNode()/makeFoo().
// All allocations use palloc() + placement new to stay within MemoryContext.
// makeNode<T>() is provided by pgcpp/parser/primnodes.h.

// Forward declaration of the hand-written scanner function (defined in
// scanner.cpp). The Bison-generated parser calls yylex(driver) to obtain
// the next token. Must be in the pgcpp_parser namespace to match the
// definition in scanner.cpp.
namespace pgcpp_parser {
BisonParser::symbol_type yylex(ParserDriver& driver);
}

static inline Node* makeStrConst(std::string str, int location) {
    Value* v = makeString(std::move(str));
    AConst* n = makeNode<AConst>();
    n->val = v;
    n->location = location;
    return n;
}

static inline Node* makeIntConst(int64_t ival, int location) {
    Value* v = makeInteger(ival);
    AConst* n = makeNode<AConst>();
    n->val = v;
    n->location = location;
    return n;
}

static inline Node* makeFloatConst(std::string fval, int location) {
    Value* v = makeFloat(std::move(fval));
    AConst* n = makeNode<AConst>();
    n->val = v;
    n->location = location;
    return n;
}

static inline Node* makeBoolAConst(bool state, int location) {
    AConst* n = makeNode<AConst>();
    n->isnull = false;
    n->isbool = true;
    n->val = makeInteger(state ? 1 : 0);
    n->location = location;
    return n;
}

static inline Node* makeNullAConst(int location) {
    AConst* n = makeNode<AConst>();
    n->isnull = true;
    n->location = location;
    return n;
}

static inline Node* makeBitStringConst(std::string str, int location) {
    Value* v = makeString(std::move(str));
    AConst* n = makeNode<AConst>();
    n->val = v;
    n->location = location;
    return n;
}

static inline Node* makeTypeCast(Node* arg, Node* type_name, int location) {
    TypeCast* n = makeNode<TypeCast>();
    n->arg = arg;
    n->type_name = static_cast<TypeName*>(type_name);
    n->location = location;
    return n;
}

static inline Alias* makeAlias(std::string aliasname, std::vector<Node*> colnames) {
    Alias* n = makeNode<Alias>();
    n->aliasname = std::move(aliasname);
    n->colnames = std::move(colnames);
    return n;
}

static inline Node* makeSimpleAExpr(AExprKind kind, std::string name,
                                    Node* lexpr, Node* rexpr, int location) {
    AExpr* n = makeNode<AExpr>();
    n->kind = kind;
    n->name.push_back(makeString(std::move(name)));
    n->lexpr = lexpr;
    n->rexpr = rexpr;
    n->location = location;
    return n;
}

static inline Node* makeAExpr(AExprKind kind, std::vector<Node*> name,
                              Node* lexpr, Node* rexpr, int location) {
    AExpr* n = makeNode<AExpr>();
    n->kind = kind;
    n->name = std::move(name);
    n->lexpr = lexpr;
    n->rexpr = rexpr;
    n->location = location;
    return n;
}

static inline Node* makeFuncCall(std::vector<Node*> funcname,
                                 std::vector<Node*> args, int location) {
    FuncCall* n = makeNode<FuncCall>();
    n->funcname = std::move(funcname);
    n->args = std::move(args);
    n->location = location;
    return n;
}

static inline Node* makeNotExpr(Node* arg) {
    return makeSimpleAExpr(AExprKind::kOp, "NOT", nullptr, arg, -1);
}

static inline Node* makeColumnRef(std::vector<Node*> fields, int location) {
    ColumnRef* n = makeNode<ColumnRef>();
    n->fields = std::move(fields);
    n->location = location;
    return n;
}

static inline Node* makeRangeVar(std::string catalogname,
                                 std::string schemaname,
                                 std::string relname, int inh, int location) {
    RangeVar* n = makeNode<RangeVar>();
    n->catalogname = std::move(catalogname);
    n->schemaname = std::move(schemaname);
    n->relname = std::move(relname);
    n->inh = (inh != 0);
    n->location = location;
    return n;
}

static inline TypeName* makeTypeName(std::vector<Node*> names, int location) {
    TypeName* n = makeNode<TypeName>();
    n->names = std::move(names);
    n->location = location;
    return n;
}

static inline TypeName* SystemTypeName(std::string name) {
    TypeName* n = makeNode<TypeName>();
    n->names.push_back(makeString("pg_catalog"));
    n->names.push_back(makeString(std::move(name)));
    n->location = -1;
    return n;
}

static inline Node* doNegate(Node* n, int location) {
    if (n != nullptr) {
        AConst* c = dynamic_cast<AConst*>(n);
        if (c != nullptr && c->val != nullptr) {
            Value* v = dynamic_cast<Value*>(c->val);
            if (v != nullptr && v->GetTag() == NodeTag::kInteger) {
                int64_t ival = v->GetInteger();
                c->val = makeInteger(-ival);
                c->location = location;
                return c;
            }
        }
    }
    return makeSimpleAExpr(AExprKind::kOp, "-", nullptr, n, location);
}

static inline Node* makeSetOp(SetOperation op, bool all,
                              Node* larg, Node* rarg) {
    SelectStmt* n = makeNode<SelectStmt>();
    n->op = op;
    n->all = all;
    n->larg = static_cast<SelectStmt*>(larg);
    n->rarg = static_cast<SelectStmt*>(rarg);
    return n;
}

static inline DefElem* makeDefElem(std::string name, Node* arg, int location) {
    DefElem* n = makeNode<DefElem>();
    n->defname = std::move(name);
    n->arg = arg;
    n->location = location;
    return n;
}

// Convert index_params (vector of IndexElem*) to a vector of String nodes
// for Constraint::keys (which stores plain column names).
static inline std::vector<Node*> indexParamsToKeys(const std::vector<Node*>& params) {
    std::vector<Node*> keys;
    keys.reserve(params.size());
    for (const auto* node : params) {
        if (auto* elem = dynamic_cast<const IndexElem*>(node)) {
            keys.push_back(makeString(elem->name));
        }
    }
    return keys;
}

// Convert key_match int (0=empty/simple, 1=FULL, 2=PARTIAL, 3=SIMPLE) to char.
static inline char keyMatchToChar(int match) {
    switch (match) {
        case 1: return 'f';  // FULL
        case 2: return 'p';  // PARTIAL
        default: return 's';  // SIMPLE (0 or 3)
    }
}
}

// ---------------------------------------------------------------------------
// Token declarations: all 461 PostgreSQL keywords.
// Organized by category (unreserved, col_name, type_func_name, reserved).
// ---------------------------------------------------------------------------

// Unreserved keywords.
%token ABORT_P ABSOLUTE_P ACCESS ACTION ADD_P ADMIN AFTER AGGREGATE ALSO ALTER
%token ALWAYS ASENSITIVE ASSERTION ASSIGNMENT AT ATOMIC ATTACH ATTRIBUTE
%token BACKWARD BEFORE BEGIN_P BREADTH BY BUFFERS CACHE CALL CALLED CASCADE CASCADED CATALOG_P
%token CHAIN CHARACTERISTICS CHECKPOINT CLASS CLOSE CLUSTER COLUMNS COMMENT
%token COMMENTS COMMIT COMMITTED COMPRESSION CONFIGURATION CONFLICT CONNECT CONNECTION
%token CONSTRAINTS CONTENT_P CONTINUE_P CONVERSION_P COPY COST COSTS CREATEDB CREATEROLE CSV CUBE
%token CURRENT_P CURSOR CYCLE DATA_P DATABASE DAY_P DEALLOCATE DECLARE DEFAULTS
%token DEFERRED DEFINER DELETE_P DELIMITER DELIMITERS DEPENDS DEPTH DETACH
%token DICTIONARY DISABLE_P DISCARD DOCUMENT_P DOMAIN_P DOUBLE_P DROP DROP_P EACH
%token ENABLE_P ENCODING ENCRYPTED END_P ENUM_P ESCAPE EVENT EXCLUDE EXCLUDING
%token EXCLUSIVE EXECUTE EXPLAIN EXPRESSION EXTENSION EXTERNAL FAMILY FILTER FORMAT
%token FINALIZE FIRST_P FOLLOWING FORCE FORWARD FUNCTION FUNCTIONS GENERATED
%token GLOBAL GRANTED GROUPS HANDLER HEADER_P HOLD HOUR_P IDENTITY_P IF_P
%token IMMEDIATE IMMUTABLE IMPLICIT_P IMPORT_P INCLUDE INCLUDING INCREMENT
%token INDEX INDEXES INHERIT INHERITS INLINE_P INPUT_P INSENSITIVE
%token INSERT INSTEAD INVOKER ISOLATION JSON KEY LABEL LANGUAGE LARGE_P LAST_P
%token LC_COLLATE_P LC_CTYPE_P LEAKPROOF LEVEL LISTEN LOAD LOCAL LOCATION LOCK_P LOCKED LOGGED LOGIN MAPPING
%token MATCH MATCHED MATERIALIZED MAXVALUE MERGE METHOD MINUTE_P MINVALUE MODE
%token MONTH_P MOVE NAME_P NAMES NEW NEXT NFC NFD NFKC NFKD NO NOCREATEDB NOCREATEROLE NOREPLICATION NOSUPERUSER NOTHING NOTIFY
%token NOWAIT NULLS_P OBJECT_P OF OFF OIDS OLD OPERATOR OPTION OPTIONS
%token ORDINALITY OTHERS OVER OVERRIDING OWNED OWNER PARALLEL PARAMETER PARSER
%token PARTIAL PARTITION PASSING PASSWORD PLANS POLICY PRECEDING PREPARE
%token PREPARED PRESERVE PRIOR PRIVILEGES PROCEDURAL PROCEDURE PROCEDURES
%token PROGRAM PUBLICATION QUOTE RANGE READ REASSIGN RECHECK RECURSIVE REF_P
%token REFERENCING REFRESH REINDEX RELATIVE_P RELEASE RENAME REPEATABLE REPLACE REPLICA
%token REPLICATION RESET RESTART RESTRICT RETURN RETURNS REVOKE ROLE ROLLBACK
%token ROLLUP ROUTINE ROUTINES ROWS RULE SAVEPOINT SCHEMA SCHEMAS SCROLL
%token SEARCH SECOND_P SECURITY SEQUENCE SEQUENCES SERIALIZABLE SERVER SESSION
%token SET SETS SETTINGS SHARE SHOW SIMPLE SKIP SNAPSHOT SQL_P STABLE STANDALONE_P START
%token STATEMENT STATISTICS STDIN STDOUT STORAGE STORED STRICT_P STRIP_P
%token SUBSCRIPTION SUMMARY SUPERUSER_P SUPPORT SYSID SYSTEM_P TABLES TABLESPACE TEMP TEMPLATE
%token TEMPORARY TEXT_P TIES TIMING TRANSACTION TRANSFORM TRIGGER TRUNCATE TRUSTED
%token TYPE_P TYPES_P UESCAPE UNBOUNDED UNCOMMITTED UNENCRYPTED UNLISTEN
%token UNLOGGED UNTIL UPDATE UNKNOWN USAGE VACUUM VALID VALIDATE VALIDATOR VALUE_P
%token VARYING VERSION_P VIEW VIEWS VOLATILE WAL WHITESPACE_P WITHIN WITHOUT WORK
%token WRAPPER WRITE XML_P YAML YEAR_P YES_P ZONE

// Column-name keywords.
%token BETWEEN BIGINT BIT BOOLEAN_P CHAR_P CHARACTER COALESCE DEC DECIMAL_P
%token EXISTS EXTRACT FLOAT_P GREATEST GROUPING INOUT INT_P INTEGER INTERVAL
%token LEAST NATIONAL NCHAR NONE NORMALIZE NORMALIZED NULLIF NUMERIC OUT_P OVERLAY
%token POSITION PRECISION REAL ROW SETOF SMALLINT SUBSTRING TIME TIMESTAMP
%token TREAT TRIM VALUES VARCHAR XMLATTRIBUTES XMLCONCAT XMLELEMENT XMLEXISTS
%token XMLFOREST XMLNAMESPACES XMLPARSE XMLPI XMLROOT XMLSERIALIZE XMLTABLE

// Type/function-name keywords.
%token AUTHORIZATION BINARY COLLATION CONCURRENTLY CROSS CURRENT_SCHEMA FREEZE
%token FULL ILIKE INNER_P IS ISNULL JOIN LEFT LIKE NATURAL NOTNULL OUTER_P
%token OVERLAPS RIGHT SIMILAR TABLESAMPLE VERBOSE

// Reserved keywords.
%token ALL ANALYSE ANALYZE AND ANY ARRAY AS ASC ASYMMETRIC BOTH CASE CAST CHECK
%token COLLATE COLUMN CONSTRAINT CREATE CURRENT_CATALOG CURRENT_DATE
%token CURRENT_ROLE CURRENT_TIME CURRENT_TIMESTAMP CURRENT_USER DEFAULT
%token DEFERRABLE DESC DISTINCT DO ELSE EXCEPT FALSE_P FETCH FOR FOREIGN FROM GRANT
%token GROUP_P HAVING IN_P INITIALLY INTERSECT INTO LATERAL_P LEADING LIMIT
%token LOCALTIME LOCALTIMESTAMP NOT NULL_P OFFSET ON ONLY OR ORDER PLACING
%token PRIMARY REFERENCES RETURNING SELECT SESSION_USER SOME SYMMETRIC TABLE
%token THEN TO TRAILING TRUE_P UNION UNIQUE USER USING VARIADIC WHEN WHERE
%token WINDOW WITH

// Non-keyword tokens.
%token <std::string> IDENT UIDENT FCONST SCONST USCONST BCONST XCONST Op
%token <int64_t> ICONST
%token <int> PARAM
%token TYPECAST DOT_DOT COLON_EQUALS EQUALS_GREATER LESS_EQUALS GREATER_EQUALS
%token NOT_EQUALS
%token NOT_LA NULLS_LA WITH_LA

// Operator precedence (lowest to highest).
%nonassoc SET				/* see relation_expr_opt_alias */
%left UNION EXCEPT
%left INTERSECT
%left OR
%left AND
%right NOT
%nonassoc IS ISNULL NOTNULL
%nonassoc '<' '>' '=' LESS_EQUALS GREATER_EQUALS NOT_EQUALS
%nonassoc BETWEEN IN_P LIKE ILIKE SIMILAR NOT_LA
%left Op
%left '+' '-'
%left '*' '/' '%'
%left '^'
%nonassoc UMINUS
%left '[' ']'
%left '(' ')'
%left '.'
%left TYPECAST
%left COLLATE

// Non-terminal type declarations.
%type <Node*> stmt stmtblock
%type <std::vector<Node*>> stmtmulti
%type <Node*> SelectStmt select_clause select_no_parens select_with_parens
%type <Node*> simple_select values_clause
%type <Node*> with_clause opt_with_clause
%type <std::vector<Node*>> cte_list
%type <Node*> common_table_expr
%type <CTEMaterialize> opt_materialized
%type <Node*> into_clause
%type <std::vector<Node*>> opt_col_names
%type <std::vector<Node*>> distinct_clause opt_distinct_clause opt_all_clause
%type <std::vector<Node*>> sort_clause opt_sort_clause sortby_list
%type <Node*> sortby
%type <SortByDir> opt_asc_desc
%type <SortByNulls> opt_nulls_order
%type <SelectLimit> select_limit opt_select_limit
%type <Node*> limit_clause offset_clause select_limit_value
%type <Node*> opt_select_fetch_first_value
%type <LimitOption> opt_limit_option
%type <GroupClause> group_clause opt_group_clause
%type <std::vector<Node*>> group_by_list
%type <Node*> group_by_item
%type <Node*> having_clause opt_having_clause
%type <std::vector<Node*>> from_clause opt_from_clause from_list
%type <Node*> table_ref joined_table
%type <Alias*> alias_clause opt_alias_clause
%type <JoinType> join_type
%type <Node*> join_qual
%type <Node*> for_locking_clause opt_for_locking_clause
%type <std::vector<Node*>> for_locking_items
%type <Node*> for_locking_item
%type <LockClauseStrength> for_locking_strength
%type <LockWaitPolicy> opt_nowait_or_skip
%type <std::vector<Node*>> window_clause opt_window_clause window_definition_list
%type <Node*> window_definition
%type <WindowDef*> window_specification
%type <Node*> over_clause opt_over_clause
%type <int> opt_frame_clause frame_extent
%type <Node*> frame_bound
%type <Node*> InsertStmt insert_target insert_rest
%type <std::vector<Node*>> insert_column_list opt_insert_column_list
%type <Node*> insert_column_item opt_on_conflict opt_conf_expr
%type <std::vector<Node*>> returning_clause opt_returning_clause
%type <Node*> DeleteStmt
%type <std::vector<Node*>> opt_using_clause using_clause_delete
%type <Node*> UpdateStmt
%type <std::vector<Node*>> set_clause_list set_target_list
%type <Node*> set_clause set_target
%type <Node*> where_clause opt_where_clause
%type <Node*> where_or_current_clause opt_where_or_current_clause
%type <Node*> relation_expr relation_expr_opt_alias qualified_name
%type <std::vector<Node*>> qualified_name_list
%type <Node*> CreateStmt ConstraintAttrElem
%type <int> OptTemp
%type <std::vector<Node*>> table_element_list opt_table_element_list
%type <Node*> columnDef table_element TableConstraint ConstraintElem
%type <std::vector<Node*>> ColQualList opt_col_qual_list
%type <Node*> ColConstraintElem ColConstraint
%type <std::vector<Node*>> ConstraintAttributeSpec opt_constraint_attr_spec
%type <int> key_match
%type <Node*> key_actions key_action_def
%type <std::vector<Node*>> OptWith opt_with reloptions opt_reloptions
%type <std::vector<Node*>> opt_inherits
%type <OnCommitAction> on_commit_opt opt_on_commit_opt
%type <std::string> OptTableSpace opt_table_access_method access_method
%type <Node*> def_elem def_arg
%type <Node*> PartitionSpec opt_partition_spec
%type <std::string> part_strategy
%type <std::vector<Node*>> part_params index_params
%type <Node*> part_elem index_elem
%type <Node*> DropStmt
%type <DropBehavior> opt_drop_behavior
%type <ObjectType> drop_type_name
%type <std::vector<Node*>> any_name_list
%type <std::vector<Node*>> any_name attrs
%type <std::vector<Node*>> target_list opt_target_list
%type <Node*> target_el
%type <Node*> a_expr b_expr c_expr
%type <Node*> indirection_el
%type <std::vector<Node*>> indirection
%type <Node*> case_expr case_arg case_default
%type <Node*> when_clause
%type <std::vector<Node*>> when_clause_list
%type <Node*> func_application func_expr func_expr_common_subexpr
%type <Node*> within_group_clause opt_within_group_clause
%type <Node*> filter_clause opt_filter_clause
%type <Node*> overlay_list position_list substr_list
%type <std::vector<Node*>> trim_list
%type <Node*> in_expr sub_type subquery_Op
%type <std::string> all_Op
%type <std::vector<Node*>> qual_Op qual_all_Op any_operator
%type <Node*> AexprConst
%type <int64_t> Iconst SignedIconst
%type <int> opt_array_bounds opt_varying opt_float
%type <std::string> Sconst
%type <Node*> row array_expr columnref
%type <Node*> Typename SimpleTypename ConstTypename GenericType Numeric
%type <Node*> Bit ConstBit Character ConstCharacter ConstDatetime ConstInterval
%type <std::vector<Node*>> func_name name_list type_list expr_list
%type <std::string> name ColId ColLabel type_function_name
%type <std::vector<Node*>> opt_type_modifiers opt_indirection
%type <std::string> unreserved_keyword col_name_keyword type_func_name_keyword
%type <std::string> reserved_keyword
%type <std::string> extract_arg
%type <Node*> func_expr_windowless
%type <std::vector<Node*>> func_arg_list
%type <Node*> func_arg_expr
%type <std::string> param_name
%type <std::vector<Node*>> columnList
%type <Node*> columnElem
%type <Node*> opt_slice_bound
%type <std::string> attr_name cursor_name relation_name
%type <int> opt_all_or_distinct
%type <Node*> OptTempTableName
%type <std::string> opt_existing_window_name
%type <std::vector<Node*>> opt_partition_clause
%type <std::vector<Node*>> reloption_list
%type <Node*> reloption_elem
%type <Node*> func_type
%type <std::string> Operator
%type <Node*> character
%type <std::vector<Node*>> opt_c_string
%type <int> opt_timezone
%type <Node*> RoleSpec
%type <std::vector<Node*>> role_list opt_role_list

// Phase 5 grammar expansion: statement types and helpers.
%type <Node*> TransactionStmt TruncateStmt ExplainStmt CommentStmt IndexStmt ViewStmt
%type <Node*> CreateAsStmt CreateSchemaStmt AlterTableStmt VacuumStmt VariableSetStmt VariableResetStmt
%type <Node*> ClusterStmt LockStmt DiscardStmt NotifyStmt ListenStmt UnlistenStmt
%type <Node*> CheckPointStmt ReindexStmt DeallocateStmt PrepareStmt ExecuteStmt
%type <Node*> LoadStmt CallStmt RenameStmt AlterOwnerStmt CreateSeqStmt AlterSeqStmt
%type <Node*> CreateFunctionStmt AlterFunctionStmt CreateTrigStmt
%type <Node*> CreateRoleStmt AlterRoleStmt DropRoleStmt GrantStmt RevokeStmt
%type <Node*> CopyStmt RefreshMatViewStmt CreateTableSpaceStmt DropTableSpaceStmt
%type <Node*> CreatedbStmt DropdbStmt AlterDatabaseStmt
%type <Node*> CreateTypeStmt CreateDomainStmt CreateCastStmt
%type <std::vector<Node*>> enum_label_list
%type <bool> opt_domain_constraints domain_constraint
%type <pgcpp::catalog::CastContext> opt_cast_context
%type <Node*> alter_table_cmd analyze_option create_as_target
%type <Node*> func_arg func_arg_info opt_createfunc_return_type
%type <std::vector<Node*>> func_as
%type <Node*> createfunc_opt_item alter_function_opt_item
%type <Node*> NumericOnly
%type <bool> opt_reindex_option
%type <Node*> privilege grantee
%type <std::string> opt_notify_payload
%type <Node*> opt_explain_stmt schema_stmt
%type <std::vector<Node*>> opt_preparable_type opt_execute_param_clause
%type <Node*> vacuum_relation set_rest
%type <std::vector<Node*>> opt_analyze_options
%type <Node*> var_value zone_value interval_qualifier

%type <std::vector<Node*>> opt_transaction_mode_list relation_expr_list analyze_options
%type <std::vector<Node*>> alter_table_cmds vacuum_relation_list var_list
%type <std::vector<Node*>> opt_column_list opt_include index_including_params
%type <std::vector<Node*>> opt_schema_elements schema_stmts
%type <std::vector<Node*>> func_args_with_defaults func_args_with_defaults_list
%type <std::vector<Node*>> func_args func_args_list func_as_list
%type <std::vector<Node*>> createfunc_opt_list opt_createfunc_opt_list
%type <std::vector<Node*>> alter_function_opt_list def_list definition opt_definition
%type <std::vector<Node*>> preparable_type_list role_options
%type <std::vector<Node*>> privileges privilege_list grantee_list function_name_list
%type <std::vector<Node*>> copy_options copy_opt_list opt_createdb_opt_list createdb_opt_list
%type <std::vector<Node*>> function_name privilege_target

%type <std::string> access_method_clause opt_index_name opt_cluster_index
%type <std::string> comment_text file_name copy_file_name
%type <std::string> iso_level nonreservedword nonreservedword_or_sconst
%type <std::string> opt_schema_name_clause

%type <bool> opt_unique opt_concurrently opt_or_replace opt_with_data
%type <bool> opt_nowait opt_program opt_restart_seqs opt_grant_option
%type <bool> copy_from

%type <int> opt_lock lock_type opt_temp

%type <ObjectType> comment_type_any_name

%type <std::pair<ReindexStmt::Kind, std::string>> reindex_target_relation

// Additional helper non-terminals.
%type <Node*> opt_transaction_mode_item SeqOptElem func_arg_with_default
%type <std::vector<Node*>> OptSeqOptList SeqOptList opt_role_options
%type <Node*> role_option
%type <bool> opt_verbose
%type <int> opt_full opt_freeze opt_check_option opt_collate_clause opt_alter_column_action opt_progress
%type <int> opt_restrict trigger_action_time trigger_events trigger_referencing trigger_for_type trigger_func_args
%type <Node*> copy_opt_item createdb_opt_item
%type <int> opt_binary opt_oids copy_delimiter opt_tablespace_owner

%%

// Top-level: a list of semicolon-separated statements.
stmtblock: stmtmulti
    {
        for (Node* rs : $1) {
            driver.parsetree.push_back(static_cast<RawStmt*>(rs));
        }
        $1.clear();
    }
;

stmtmulti: stmtmulti ';' stmt
    {
        if ($3 != nullptr) {
            RawStmt* n = makeNode<RawStmt>();
            n->stmt = $3;
            n->stmt_location = 0;
            n->stmt_len = 0;
            $1.push_back(n);
        }
        $$ = std::move($1);
    }
    | stmt
    {
        if ($1 != nullptr) {
            RawStmt* n = makeNode<RawStmt>();
            n->stmt = $1;
            n->stmt_location = 0;
            n->stmt_len = 0;
            $$.push_back(n);
        }
    }
;

stmt:
      SelectStmt
    | InsertStmt
    | UpdateStmt
    | DeleteStmt
    | CreateStmt
    | CreateAsStmt
    | CreateSchemaStmt
    | AlterTableStmt
    | DropStmt
    | TruncateStmt
    | CommentStmt
    | IndexStmt
    | ViewStmt
    | TransactionStmt
    | ExplainStmt
    | VacuumStmt
    | VariableSetStmt
    | VariableResetStmt
    | ClusterStmt
    | LockStmt
    | DiscardStmt
    | NotifyStmt
    | ListenStmt
    | UnlistenStmt
    | CheckPointStmt
    | ReindexStmt
    | DeallocateStmt
    | PrepareStmt
    | ExecuteStmt
    | LoadStmt
    | CallStmt
    | RenameStmt
    | AlterOwnerStmt
    | CreateSeqStmt
    | AlterSeqStmt
    | CreateFunctionStmt
    | AlterFunctionStmt
    | CreateTrigStmt
    | CreateRoleStmt
    | AlterRoleStmt
    | DropRoleStmt
    | GrantStmt
    | RevokeStmt
    | CopyStmt
    | RefreshMatViewStmt
    | CreateTableSpaceStmt
    | DropTableSpaceStmt
    | CreatedbStmt
    | DropdbStmt
    | AlterDatabaseStmt
    | CreateTypeStmt
    | CreateDomainStmt
    | CreateCastStmt
    | /* empty */
        { $$ = nullptr; }
;

// CREATE TYPE name AS ENUM ('label', ...)
CreateTypeStmt:
      CREATE TYPE_P any_name AS ENUM_P '(' enum_label_list ')'
        {
            CreateTypeStmt* n = makeNode<CreateTypeStmt>();
            n->type_name = std::move($3);
            for (Node* label_node : $7) {
                auto* v = dynamic_cast<Value*>(label_node);
                if (v != nullptr)
                    n->labels.push_back(v->GetString());
            }
            $$ = n;
        }
;

enum_label_list:
      Sconst
        { $$.push_back(makeString($1)); }
    | enum_label_list ',' Sconst
        { $1.push_back(makeString($3)); $$ = std::move($1); }
;

// CREATE DOMAIN name AS type [constraints]
CreateDomainStmt:
      CREATE DOMAIN_P any_name AS Typename opt_domain_constraints
        {
            CreateDomainStmt* n = makeNode<CreateDomainStmt>();
            n->domainname = std::move($3);
            n->type_name = static_cast<TypeName*>($5);
            // Simplified: only NOT NULL constraint is supported.
            n->not_null = $6;
            $$ = n;
        }
;

opt_domain_constraints:
      /* empty */
        { $$ = false; }
    | opt_domain_constraints domain_constraint
        { $$ = $1 || $2; }
;

domain_constraint:
      NOT NULL_P
        { $$ = true; }
    | NULL_P
        { $$ = false; }
;

// CREATE CAST (source_type AS target_type) WITH FUNCTION func_name | WITHOUT FUNCTION
CreateCastStmt:
      CREATE CAST '(' Typename AS Typename ')' WITH FUNCTION function_name opt_cast_context
        {
            CreateCastStmt* n = makeNode<CreateCastStmt>();
            n->sourcetype = static_cast<TypeName*>($4);
            n->targettype = static_cast<TypeName*>($6);
            n->func = std::move($10);
            n->without_function = false;
            n->context = $11;
            $$ = n;
        }
    | CREATE CAST '(' Typename AS Typename ')' WITHOUT FUNCTION opt_cast_context
        {
            CreateCastStmt* n = makeNode<CreateCastStmt>();
            n->sourcetype = static_cast<TypeName*>($4);
            n->targettype = static_cast<TypeName*>($6);
            n->without_function = true;
            n->context = $10;
            $$ = n;
        }
;

opt_cast_context:
      /* empty */
        { $$ = pgcpp::catalog::CastContext::kExplicit; }
    | AS ASSIGNMENT
        { $$ = pgcpp::catalog::CastContext::kAssignment; }
    | AS IMPLICIT_P
        { $$ = pgcpp::catalog::CastContext::kImplicit; }
;

// SELECT statement.
SelectStmt:
      select_no_parens
    | select_with_parens
    | with_clause select_no_parens
        {
            // Leading WITH clause for SELECT, e.g.:
            //   WITH cte AS (SELECT 1) SELECT * FROM cte
            // Standard SQL form — the WITH clause appears before SELECT.
            SelectStmt* sel = static_cast<SelectStmt*>($2);
            sel->with_clause = static_cast<WithClause*>($1);
            $$ = sel;
        }
;

select_with_parens:
      '(' select_no_parens ')'
        { $$ = $2; }
    | '(' select_with_parens ')'
        { $$ = $2; }
;

select_no_parens:
      simple_select opt_sort_clause opt_for_locking_clause opt_select_limit opt_with_clause
        {
            SelectStmt* s = static_cast<SelectStmt*>($1);
            s->sort_clause = std::move($2);
            (void)$3;
            s->limit_offset = $4.limit_offset;
            s->limit_count = $4.limit_count;
            s->limit_option = $4.limit_option;
            s->with_clause = static_cast<WithClause*>($5);
            $$ = $1;
        }
;

select_clause:
      simple_select
    | select_with_parens
;

simple_select:
      SELECT opt_all_clause opt_target_list into_clause opt_from_clause
          opt_where_clause opt_group_clause opt_having_clause opt_window_clause
        {
            SelectStmt* n = makeNode<SelectStmt>();
            n->distinct_clause = std::move($2);
            n->target_list = std::move($3);
            n->into_clause = static_cast<IntoClause*>($4);
            n->from_clause = std::move($5);
            n->where_clause = $6;
            n->group_clause = $7.group_clause;
            n->group_distinct = $7.group_distinct;
            n->having_clause = $8;
            n->window_clause = std::move($9);
            $$ = n;
        }
    | SELECT distinct_clause target_list into_clause opt_from_clause
          opt_where_clause opt_group_clause opt_having_clause opt_window_clause
        {
            SelectStmt* n = makeNode<SelectStmt>();
            n->distinct_clause = std::move($2);
            n->target_list = std::move($3);
            n->into_clause = static_cast<IntoClause*>($4);
            n->from_clause = std::move($5);
            n->where_clause = $6;
            n->group_clause = $7.group_clause;
            n->group_distinct = $7.group_distinct;
            n->having_clause = $8;
            n->window_clause = std::move($9);
            $$ = n;
        }
    | values_clause
    | select_clause UNION opt_all_or_distinct select_clause
        { $$ = makeSetOp(SetOperation::kUnion, $3 != 0, $1, $4); }
    | select_clause INTERSECT opt_all_or_distinct select_clause
        { $$ = makeSetOp(SetOperation::kIntersect, $3 != 0, $1, $4); }
    | select_clause EXCEPT opt_all_or_distinct select_clause
        { $$ = makeSetOp(SetOperation::kExcept, $3 != 0, $1, $4); }
;

opt_all_or_distinct:
      ALL
        { $$ = 1; }
    | DISTINCT
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

opt_target_list:
      target_list
    | /* empty */
        { $$ = {}; }
;

values_clause:
      VALUES '(' expr_list ')'
        {
            SelectStmt* n = makeNode<SelectStmt>();
            n->values_lists.push_back($3);
            $$ = n;
        }
    | values_clause ',' '(' expr_list ')'
        {
            SelectStmt* n = static_cast<SelectStmt*>($1);
            n->values_lists.push_back($4);
            $$ = $1;
        }
;

// WITH clause.
with_clause:
      WITH cte_list
        {
            WithClause* n = makeNode<WithClause>();
            n->ctes = std::move($2);
            n->location = @1;
            $$ = n;
        }
    | WITH RECURSIVE cte_list
        {
            WithClause* n = makeNode<WithClause>();
            n->ctes = std::move($3);
            n->recursive = true;
            n->location = @1;
            $$ = n;
        }
;

opt_with_clause:
      with_clause
    | /* empty */
        { $$ = nullptr; }
;

cte_list:
      common_table_expr
        { $$.push_back($1); }
    | cte_list ',' common_table_expr
        { $1.push_back($3); $$ = std::move($1); }
;

common_table_expr:
      name opt_materialized AS '(' select_no_parens ')'
        {
            CommonTableExpr* n = makeNode<CommonTableExpr>();
            n->ctename = $1;
            n->ctematerialized = $2;
            n->ctequery = $5;
            n->location = @1;
            $$ = n;
        }
    | name opt_materialized AS '(' select_with_parens ')'
        {
            CommonTableExpr* n = makeNode<CommonTableExpr>();
            n->ctename = $1;
            n->ctematerialized = $2;
            n->ctequery = $5;
            n->location = @1;
            $$ = n;
        }
    | name opt_col_names opt_materialized AS '(' select_no_parens ')'
        {
            CommonTableExpr* n = makeNode<CommonTableExpr>();
            n->ctename = $1;
            n->aliascolnames = std::move($2);
            n->ctematerialized = $3;
            n->ctequery = $6;
            n->location = @1;
            $$ = n;
        }
;

opt_materialized:
      MATERIALIZED
        { $$ = CTEMaterialize::kAlways; }
    | NOT MATERIALIZED
        { $$ = CTEMaterialize::kNever; }
    | /* empty */
        { $$ = CTEMaterialize::kDefault; }
;

// SELECT INTO.
into_clause:
      INTO OptTempTableName opt_col_names opt_reloptions opt_on_commit_opt OptTableSpace
        {
            IntoClause* n = makeNode<IntoClause>();
            n->rel = static_cast<RangeVar*>($2);
            n->col_names = std::move($3);
            n->options = std::move($4);
            n->on_commit = $5;
            n->table_space_name = $6;
            $$ = n;
        }
    | /* empty */
        { $$ = nullptr; }
;

OptTempTableName:
      relation_name
        { $$ = makeRangeVar("", "", $1, true, @1); }
    | TEMP relation_name
        { $$ = makeRangeVar("", "", $2, true, @2); }
    | TEMPORARY relation_name
        { $$ = makeRangeVar("", "", $2, true, @2); }
    | UNLOGGED relation_name
        { $$ = makeRangeVar("", "", $2, true, @2); }
;

relation_name:
      ColId
;

opt_col_names:
      '(' columnList ')'
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

// DISTINCT / ALL.
distinct_clause:
      DISTINCT
        { $$ = std::vector<Node*>{nullptr}; }
    | DISTINCT ON '(' expr_list ')'
        { $$ = std::move($4); }
;

opt_all_clause:
      ALL
        { $$ = {}; }
    | /* empty */
        { $$ = {}; }
;

opt_distinct_clause:
      distinct_clause
    | opt_all_clause
;

// ORDER BY.
sort_clause:
      ORDER BY sortby_list
        { $$ = std::move($3); }
;

opt_sort_clause:
      sort_clause
    | /* empty */
        { $$ = {}; }
;

sortby_list:
      sortby
        { $$.push_back($1); }
    | sortby_list ',' sortby
        { $1.push_back($3); $$ = std::move($1); }
;

sortby:
      a_expr opt_asc_desc opt_nulls_order
        {
            SortBy* n = makeNode<SortBy>();
            n->node = $1;
            n->sortby_dir = $2;
            n->sortby_nulls = $3;
            n->location = @1;
            $$ = n;
        }
    | a_expr USING qual_all_Op
        {
            SortBy* n = makeNode<SortBy>();
            n->node = $1;
            n->sortby_dir = SortByDir::kUsing;
            n->use_op = std::move($3);
            n->location = @1;
            $$ = n;
        }
;

qual_all_Op:
      all_Op
        { $$.push_back(makeString($1)); }
    | OPERATOR '(' any_operator ')'
        { $$ = std::move($3); }
;

any_operator:
      all_Op
        { $$.push_back(makeString($1)); }
    | any_operator '.' all_Op
        { $1.push_back(makeString($3)); $$ = std::move($1); }
;

opt_asc_desc:
      ASC
        { $$ = SortByDir::kAsc; }
    | DESC
        { $$ = SortByDir::kDesc; }
    | /* empty */
        { $$ = SortByDir::kDefault; }
;

opt_nulls_order:
      NULLS_P FIRST_P
        { $$ = SortByNulls::kFirst; }
    | NULLS_P LAST_P
        { $$ = SortByNulls::kLast; }
    | /* empty */
        { $$ = SortByNulls::kDefault; }
;

// LIMIT / OFFSET.
select_limit:
      limit_clause offset_clause
        {
            SelectLimit s;
            s.limit_count = $1;
            s.limit_offset = $2;
            $$ = s;
        }
    | offset_clause limit_clause
        {
            SelectLimit s;
            s.limit_offset = $1;
            s.limit_count = $2;
            $$ = s;
        }
    | limit_clause
        {
            SelectLimit s;
            s.limit_count = $1;
            $$ = s;
        }
    | offset_clause
        {
            SelectLimit s;
            s.limit_offset = $1;
            $$ = s;
        }
;

opt_select_limit:
      select_limit
    | /* empty */
        {
            SelectLimit s;
            $$ = s;
        }
;

limit_clause:
      LIMIT select_limit_value opt_limit_option
        { $$ = $2; }
    | FETCH FIRST_P opt_select_fetch_first_value opt_limit_option
        { $$ = $3; }
    | FETCH NEXT opt_select_fetch_first_value opt_limit_option
        { $$ = $3; }
;

offset_clause:
      OFFSET select_limit_value
        { $$ = $2; }
    | OFFSET select_limit_value ROW
        { $$ = $2; }
    | OFFSET select_limit_value ROWS
        { $$ = $2; }
;

select_limit_value:
      a_expr
    | ALL
        { $$ = makeNullAConst(@1); }
;

opt_select_fetch_first_value:
      SignedIconst
        { $$ = makeIntConst($1, @1); }
    | a_expr
    | /* empty */
        { $$ = nullptr; }
;

opt_limit_option:
      WITH TIES
        { $$ = LimitOption::kWithTies; }
    | /* empty */
        { $$ = LimitOption::kCount; }
;

// GROUP BY / HAVING.
group_clause:
      GROUP_P BY group_by_list
        {
            GroupClause g;
            g.group_clause = std::move($3);
            $$ = g;
        }
    | GROUP_P BY DISTINCT group_by_list
        {
            GroupClause g;
            g.group_clause = std::move($4);
            g.group_distinct = true;
            $$ = g;
        }
;

opt_group_clause:
      group_clause
    | /* empty */
        {
            GroupClause g;
            $$ = g;
        }
;

group_by_list:
      group_by_item
        { $$.push_back($1); }
    | group_by_list ',' group_by_item
        { $1.push_back($3); $$ = std::move($1); }
;

group_by_item:
      a_expr
        {
            SortBy* n = makeNode<SortBy>();
            n->node = $1;
            n->sortby_dir = SortByDir::kDefault;
            n->sortby_nulls = SortByNulls::kDefault;
            n->location = @1;
            $$ = n;
        }
    | '(' ')'
        {
            GroupingSet* n = makeNode<GroupingSet>();
            n->kind = GroupingSetKind::kEmpty;
            n->location = @1;
            $$ = n;
        }
    | ROLLUP '(' expr_list ')'
        {
            GroupingSet* n = makeNode<GroupingSet>();
            n->kind = GroupingSetKind::kRollup;
            n->content = std::move($3);
            n->location = @1;
            $$ = n;
        }
    | CUBE '(' expr_list ')'
        {
            GroupingSet* n = makeNode<GroupingSet>();
            n->kind = GroupingSetKind::kCube;
            n->content = std::move($3);
            n->location = @1;
            $$ = n;
        }
    | GROUPING SETS '(' group_by_list ')'
        {
            GroupingSet* n = makeNode<GroupingSet>();
            n->kind = GroupingSetKind::kSets;
            n->content = std::move($4);
            n->location = @1;
            $$ = n;
        }
;

having_clause:
      HAVING a_expr
        { $$ = $2; }
;

opt_having_clause:
      having_clause
    | /* empty */
        { $$ = nullptr; }
;

// FROM clause.
from_clause:
      FROM from_list
        { $$ = std::move($2); }
;

opt_from_clause:
      from_clause
    | /* empty */
        { $$ = {}; }
;

from_list:
      table_ref
        { $$.push_back($1); }
    | from_list ',' table_ref
        { $1.push_back($3); $$ = std::move($1); }
;

table_ref:
      relation_expr opt_alias_clause
        {
            static_cast<RangeVar*>($1)->alias = $2;
            $$ = $1;
        }
    | relation_expr alias_clause
        {
            static_cast<RangeVar*>($1)->alias = $2;
            $$ = $1;
        }
    | relation_expr opt_alias_clause join_type JOIN table_ref join_qual
        {
            if ($2 != nullptr) static_cast<RangeVar*>($1)->alias = $2;
            JoinExpr* n = makeNode<JoinExpr>();
            n->jointype = $3;
            n->is_natural = false;
            n->larg = $1;
            n->rarg = $5;
            n->quals = $6;
            $$ = n;
        }
    | relation_expr opt_alias_clause NATURAL join_type JOIN table_ref
        {
            if ($2 != nullptr) static_cast<RangeVar*>($1)->alias = $2;
            JoinExpr* n = makeNode<JoinExpr>();
            n->jointype = $4;
            n->is_natural = true;
            n->larg = $1;
            n->rarg = $6;
            $$ = n;
        }
    | relation_expr opt_alias_clause CROSS JOIN table_ref
        {
            if ($2 != nullptr) static_cast<RangeVar*>($1)->alias = $2;
            JoinExpr* n = makeNode<JoinExpr>();
            n->jointype = JoinType::kInner;
            n->is_natural = false;
            n->larg = $1;
            n->rarg = $5;
            $$ = n;
        }
    | '(' select_clause ')' alias_clause
        {
            RangeSubselect* n = makeNode<RangeSubselect>();
            n->subquery = $2;
            n->alias = $4;
            $$ = n;
        }
;

joined_table:
      '(' joined_table ')'
        { $$ = $2; }
    | table_ref join_type JOIN table_ref join_qual
        {
            JoinExpr* n = makeNode<JoinExpr>();
            n->jointype = $2;
            n->is_natural = false;
            n->larg = $1;
            n->rarg = $4;
            n->quals = $5;
            $$ = n;
        }
    | table_ref NATURAL join_type JOIN table_ref
        {
            JoinExpr* n = makeNode<JoinExpr>();
            n->jointype = $3;
            n->is_natural = true;
            n->larg = $1;
            n->rarg = $5;
            $$ = n;
        }
    | table_ref CROSS JOIN table_ref
        {
            JoinExpr* n = makeNode<JoinExpr>();
            n->jointype = JoinType::kInner;
            n->is_natural = false;
            n->larg = $1;
            n->rarg = $4;
            $$ = n;
        }
;

alias_clause:
      AS ColId
        { $$ = makeAlias($2, {}); }
    | AS ColId '(' columnList ')'
        { $$ = makeAlias($2, std::move($4)); }
    | ColId
        { $$ = makeAlias($1, {}); }
    | ColId '(' columnList ')'
        { $$ = makeAlias($1, std::move($3)); }
;

opt_alias_clause:
      alias_clause
    | /* empty */
        { $$ = nullptr; }
;

join_type:
      INNER_P
        { $$ = JoinType::kInner; }
    | LEFT
        { $$ = JoinType::kLeft; }
    | RIGHT
        { $$ = JoinType::kRight; }
    | FULL
        { $$ = JoinType::kFull; }
    | /* empty */
        { $$ = JoinType::kInner; }
;

join_qual:
      USING '(' name_list ')'
        { $$ = nullptr; }
    | ON a_expr
        { $$ = $2; }
    | /* empty */
        { $$ = nullptr; }
;

// FOR UPDATE / SHARE.
for_locking_clause:
      for_locking_strength for_locking_items opt_nowait_or_skip
        {
            LockingClause* n = makeNode<LockingClause>();
            n->locked_rels = std::move($2);
            n->strength = $1;
            n->wait_policy = $3;
            $$ = n;
        }
    | for_locking_strength
        {
            LockingClause* n = makeNode<LockingClause>();
            n->strength = $1;
            $$ = n;
        }
;

opt_for_locking_clause:
      for_locking_clause
    | /* empty */
        { $$ = nullptr; }
;

for_locking_items:
      for_locking_item
        { $$.push_back($1); }
    | for_locking_items ',' for_locking_item
        { $1.push_back($3); $$ = std::move($1); }
;

for_locking_item:
      qualified_name
        { $$ = $1; }
;

for_locking_strength:
      FOR UPDATE
        { $$ = LockClauseStrength::kForUpdate; }
    | FOR NO KEY UPDATE
        { $$ = LockClauseStrength::kForNoKeyUpdate; }
    | FOR SHARE
        { $$ = LockClauseStrength::kForShare; }
    | FOR KEY SHARE
        { $$ = LockClauseStrength::kForKeyShare; }
;

opt_nowait_or_skip:
      NOWAIT
        { $$ = LockWaitPolicy::kSkip; }
    | SKIP LOCKED
        { $$ = LockWaitPolicy::kSkip; }
    | /* empty */
        { $$ = LockWaitPolicy::kBlock; }
;

// WINDOW clause.
window_clause:
      WINDOW window_definition_list
        { $$ = std::move($2); }
;

opt_window_clause:
      window_clause
    | /* empty */
        { $$ = {}; }
;

window_definition_list:
      window_definition
        { $$.push_back($1); }
    | window_definition_list ',' window_definition
        { $1.push_back($3); $$ = std::move($1); }
;

window_definition:
      ColId AS window_specification
        {
            WindowDef* n = $3;
            n->name = $1;
            n->location = @1;
            $$ = n;
        }
;

window_specification:
      '(' opt_existing_window_name opt_partition_clause opt_sort_clause opt_frame_clause ')'
        {
            WindowDef* n = makeNode<WindowDef>();
            n->refname = $2;
            n->partition_clause = std::move($3);
            n->order_clause = std::move($4);
            n->frame_options = $5;
            n->location = @1;
            $$ = n;
        }
;

opt_existing_window_name:
      ColId
    | /* empty */
        { $$ = ""; }
;

opt_partition_clause:
      PARTITION BY expr_list
        { $$ = std::move($3); }
    | /* empty */
        { $$ = {}; }
;

over_clause:
      OVER window_specification
        {
            $2->location = @1;
            $$ = $2;
        }
    | OVER ColId
        {
            WindowDef* n = makeNode<WindowDef>();
            n->refname = $2;
            n->location = @1;
            $$ = n;
        }
;

opt_over_clause:
      over_clause
    | /* empty */
        { $$ = nullptr; }
;

opt_frame_clause:
      RANGE frame_extent
        { $$ = $2; }
    | ROWS frame_extent
        { $$ = $2; }
    | GROUPS frame_extent
        { $$ = $2; }
    | /* empty */
        { $$ = 0; }
;

frame_extent:
      frame_bound
        { $$ = 1; (void)$1; }
    | BETWEEN frame_bound AND frame_bound
        { $$ = 2; (void)$2; (void)$4; }
;

frame_bound:
      UNBOUNDED PRECEDING
        { $$ = nullptr; }
    | UNBOUNDED FOLLOWING
        { $$ = nullptr; }
    | CURRENT_P ROW
        { $$ = nullptr; }
    | a_expr PRECEDING
        { $$ = $1; }
    | a_expr FOLLOWING
        { $$ = $1; }
;

// INSERT statement.
InsertStmt:
      opt_with_clause INSERT INTO insert_target insert_rest opt_on_conflict opt_returning_clause
        {
            InsertStmt* n = makeNode<InsertStmt>();
            n->relation = static_cast<RangeVar*>($4);
            InsertStmt* rest = static_cast<InsertStmt*>($5);
            n->cols = std::move(rest->cols);
            n->select_stmt = rest->select_stmt;
            n->on_conflict_clause = static_cast<OnConflictClause*>($6);
            n->returning_list = std::move($7);
            n->with_clause = static_cast<WithClause*>($1);
            $$ = n;
        }
;

insert_target:
      qualified_name
        { $$ = $1; }
;

insert_rest:
      SelectStmt
        {
            InsertStmt* n = makeNode<InsertStmt>();
            n->select_stmt = $1;
            $$ = n;
        }
    | '(' insert_column_list ')' SelectStmt
        {
            InsertStmt* n = makeNode<InsertStmt>();
            n->cols = std::move($2);
            n->select_stmt = $4;
            $$ = n;
        }
    | DEFAULT VALUES
        {
            InsertStmt* n = makeNode<InsertStmt>();
            $$ = n;
        }
;

opt_insert_column_list:
      insert_column_list
    | /* empty */
        { $$ = {}; }
;

insert_column_list:
      insert_column_item
        { $$.push_back($1); }
    | insert_column_list ',' insert_column_item
        { $1.push_back($3); $$ = std::move($1); }
;

insert_column_item:
      ColId opt_indirection
        {
            ResTarget* n = makeNode<ResTarget>();
            n->name = $1;
            n->indirection = std::move($2);
            n->location = @1;
            $$ = n;
        }
;

opt_indirection:
      /* empty */
        { $$ = {}; }
    | opt_indirection indirection_el
        { $1.push_back($2); $$ = std::move($1); }
;

opt_on_conflict:
      ON CONFLICT opt_conf_expr
        { $$ = $3; }
    | /* empty */
        { $$ = nullptr; }
;

opt_conf_expr:
      '(' index_params ')' DO NOTHING
        {
            OnConflictClause* n = makeNode<OnConflictClause>();
            n->action = OnConflictAction::kNothing;
            n->infer = makeNode<InferClause>();
            n->infer->index_elems = std::move($2);
            n->location = @1;
            $$ = n;
        }
    | '(' index_params ')' DO UPDATE SET set_clause_list opt_where_clause
        {
            OnConflictClause* n = makeNode<OnConflictClause>();
            n->action = OnConflictAction::kUpdate;
            n->infer = makeNode<InferClause>();
            n->infer->index_elems = std::move($2);
            n->target_list = std::move($7);
            n->where_clause = $8;
            n->location = @1;
            $$ = n;
        }
    | DO NOTHING
        {
            OnConflictClause* n = makeNode<OnConflictClause>();
            n->action = OnConflictAction::kNothing;
            n->location = @1;
            $$ = n;
        }
    | DO UPDATE SET set_clause_list opt_where_clause
        {
            OnConflictClause* n = makeNode<OnConflictClause>();
            n->action = OnConflictAction::kUpdate;
            n->target_list = std::move($4);
            n->where_clause = $5;
            n->location = @1;
            $$ = n;
        }
;

returning_clause:
      RETURNING target_list
        { $$ = std::move($2); }
;

opt_returning_clause:
      returning_clause
    | /* empty */
        { $$ = {}; }
;

// DELETE statement.
DeleteStmt:
      opt_with_clause DELETE_P FROM relation_expr_opt_alias opt_using_clause
          opt_where_or_current_clause opt_returning_clause
        {
            DeleteStmt* n = makeNode<DeleteStmt>();
            n->relation = static_cast<RangeVar*>($4);
            n->using_clause = std::move($5);
            n->where_clause = $6;
            n->returning_list = std::move($7);
            n->with_clause = static_cast<WithClause*>($1);
            $$ = n;
        }
;

opt_using_clause:
      using_clause_delete
    | /* empty */
        { $$ = {}; }
;

using_clause_delete:
      USING from_list
        { $$ = std::move($2); }
;

// UPDATE statement.
UpdateStmt:
      opt_with_clause UPDATE relation_expr_opt_alias
          SET set_clause_list opt_from_clause opt_where_or_current_clause opt_returning_clause
        {
            UpdateStmt* n = makeNode<UpdateStmt>();
            n->relation = static_cast<RangeVar*>($3);
            n->target_list = std::move($5);
            n->from_clause = std::move($6);
            n->where_clause = $7;
            n->returning_list = std::move($8);
            n->with_clause = static_cast<WithClause*>($1);
            $$ = n;
        }
;

set_clause_list:
      set_clause
        { $$.push_back($1); }
    | set_clause_list ',' set_clause
        { $1.push_back($3); $$ = std::move($1); }
;

set_clause:
      set_target '=' a_expr
        {
            static_cast<ResTarget*>($1)->val = $3;
            $$ = $1;
        }
    | '(' set_target_list ')' '=' a_expr
        {
            if (!$2.empty()) {
                ResTarget* rt = static_cast<ResTarget*>($2[0]);
                MultiAssignRef* ma = makeNode<MultiAssignRef>();
                ma->source = $5;
                ma->colno = 1;
                ma->ncolumns = static_cast<int>($2.size());
                rt->val = ma;
                $$ = $2[0];
            } else {
                $$ = nullptr;
            }
        }
;

set_target:
      ColId opt_indirection
        {
            ResTarget* n = makeNode<ResTarget>();
            n->name = $1;
            n->indirection = std::move($2);
            n->location = @1;
            $$ = n;
        }
;

set_target_list:
      set_target
        { $$.push_back($1); }
    | set_target_list ',' set_target
        { $1.push_back($3); $$ = std::move($1); }
;

// WHERE clause.
where_clause:
      WHERE a_expr
        { $$ = $2; }
;

opt_where_clause:
      where_clause
    | /* empty */
        { $$ = nullptr; }
;

where_or_current_clause:
      WHERE CURRENT_P OF cursor_name
        { $$ = nullptr; }
    | WHERE a_expr
        { $$ = $2; }
;

opt_where_or_current_clause:
      where_or_current_clause
    | /* empty */
        { $$ = nullptr; }
;

cursor_name:
      ColId
        { $$ = $1; }
;

// Relation references.
relation_expr:
      qualified_name
        { $$ = $1; }
    | qualified_name '*'
        {
            static_cast<RangeVar*>($1)->inh = true;
            $$ = $1;
        }
    | ONLY qualified_name
        {
            static_cast<RangeVar*>($2)->inh = false;
            $$ = $2;
        }
    | ONLY '(' qualified_name ')'
        {
            static_cast<RangeVar*>($3)->inh = false;
            $$ = $3;
        }
;

relation_expr_opt_alias:
      relation_expr %prec UMINUS
        { $$ = $1; }
    | relation_expr AS ColId
        {
            static_cast<RangeVar*>($1)->alias = makeAlias($3, {});
            $$ = $1;
        }
    | relation_expr ColId
        {
            static_cast<RangeVar*>($1)->alias = makeAlias($2, {});
            $$ = $1;
        }
;

qualified_name:
      ColId
        { $$ = makeRangeVar("", "", $1, true, @1); }
    | ColId '.' ColId
        { $$ = makeRangeVar("", $1, $3, true, @1); }
    | ColId '.' ColId '.' ColId
        { $$ = makeRangeVar($1, $3, $5, true, @1); }
;

qualified_name_list:
      qualified_name
        { $$.push_back($1); }
    | qualified_name_list ',' qualified_name
        { $1.push_back($3); $$ = std::move($1); }
;

// CREATE TABLE.
CreateStmt:
      CREATE OptTemp TABLE qualified_name '(' opt_table_element_list ')'
          opt_inherits opt_partition_spec opt_with opt_on_commit_opt
          opt_table_access_method OptTableSpace
        {
            CreateStmt* n = makeNode<CreateStmt>();
            n->relation = static_cast<RangeVar*>($4);
            n->table_elts = std::move($6);
            n->inh_relations = std::move($8);
            n->partspec = static_cast<PartitionSpec*>($9);
            n->options = std::move($10);
            n->oncommit = $11;
            n->access_method = $12;
            n->tablespacename = $13;
            $$ = n;
        }
    | CREATE OptTemp TABLE IF_P NOT EXISTS qualified_name '(' opt_table_element_list ')'
          opt_inherits opt_partition_spec opt_with opt_on_commit_opt
          opt_table_access_method OptTableSpace
        {
            CreateStmt* n = makeNode<CreateStmt>();
            n->relation = static_cast<RangeVar*>($7);
            n->table_elts = std::move($9);
            n->inh_relations = std::move($11);
            n->partspec = static_cast<PartitionSpec*>($12);
            n->options = std::move($13);
            n->oncommit = $14;
            n->access_method = $15;
            n->tablespacename = $16;
            n->if_not_exists = true;
            $$ = n;
        }
;

OptTemp:
      TEMPORARY
        { $$ = 1; }
    | TEMP
        { $$ = 1; }
    | /* empty */
        { $$ = 0; }
;

opt_table_element_list:
      table_element_list
    | /* empty */
        { $$ = {}; }
;

table_element_list:
      table_element
        { $$.push_back($1); }
    | table_element_list ',' table_element
        { $1.push_back($3); $$ = std::move($1); }
;

table_element:
      columnDef
    | TableConstraint
;

columnDef:
      ColId Typename opt_col_qual_list
        {
            ColumnDef* n = makeNode<ColumnDef>();
            n->colname = $1;
            n->type_name = static_cast<TypeName*>($2);
            n->constraints = std::move($3);
            n->is_local = true;
            n->location = @1;
            $$ = n;
        }
;

opt_col_qual_list:
      ColQualList
    | /* empty */
        { $$ = {}; }
;

ColQualList:
      ColQualList ColConstraintElem
        { $1.push_back($2); $$ = std::move($1); }
    | ColConstraintElem
        { $$.push_back($1); }
;

ColConstraintElem:
      CONSTRAINT name ColConstraint
        {
            auto* c = static_cast<Constraint*>($3);
            c->conname = $2;
            $$ = $3;
        }
    | ColConstraint
        { $$ = $1; }
;

ColConstraint:
      NOT NULL_P
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kNotNull;
            n->location = @1;
            $$ = n;
        }
    | NULL_P
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kNull;
            n->location = @1;
            $$ = n;
        }
    | DEFAULT b_expr
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kDefault;
            n->raw_expr = $2;
            n->location = @1;
            $$ = n;
        }
    | PRIMARY KEY opt_constraint_attr_spec
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kPrimary;
            n->location = @1;
            $$ = n;
        }
    | UNIQUE opt_constraint_attr_spec
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kUnique;
            n->location = @1;
            $$ = n;
        }
    | CHECK '(' a_expr ')' opt_constraint_attr_spec
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kCheck;
            n->raw_expr = $3;
            n->location = @1;
            $$ = n;
        }
    | REFERENCES qualified_name opt_column_list key_match key_actions
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kForeign;
            n->pktable = static_cast<RangeVar*>($2);
            n->fk_attrs = std::move($3);
            n->fk_matchtype = keyMatchToChar($4);
            n->location = @1;
            $$ = n;
        }
;

TableConstraint:
      CONSTRAINT name ConstraintElem
        {
            auto* c = static_cast<Constraint*>($3);
            c->conname = $2;
            $$ = $3;
        }
    | ConstraintElem
        { $$ = $1; }
;

ConstraintElem:
      CHECK '(' a_expr ')' ConstraintAttributeSpec
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kCheck;
            n->raw_expr = $3;
            n->location = @1;
            $$ = n;
        }
    | PRIMARY KEY '(' index_params ')' ConstraintAttributeSpec
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kPrimary;
            n->keys = indexParamsToKeys($4);
            n->location = @1;
            $$ = n;
        }
    | UNIQUE '(' index_params ')' ConstraintAttributeSpec
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kUnique;
            n->keys = indexParamsToKeys($3);
            n->location = @1;
            $$ = n;
        }
    | FOREIGN KEY '(' index_params ')' REFERENCES qualified_name
          opt_column_list key_match key_actions ConstraintAttributeSpec
        {
            auto* n = makeNode<Constraint>();
            n->contype = ConstrType::kForeign;
            n->keys = indexParamsToKeys($4);
            n->pktable = static_cast<RangeVar*>($7);
            n->fk_attrs = std::move($8);
            n->fk_matchtype = keyMatchToChar($9);
            n->location = @1;
            $$ = n;
        }
;

ConstraintAttributeSpec:
      ConstraintAttributeSpec ConstraintAttrElem
        { $1.push_back($2); $$ = std::move($1); }
    | /* empty */
        { $$ = {}; }
;

ConstraintAttrElem:
      DEFERRABLE
        { $$ = makeString("deferrable"); }
    | NOT DEFERRABLE
        { $$ = makeString("not_deferrable"); }
    | INITIALLY DEFERRED
        { $$ = makeString("initially_deferred"); }
    | INITIALLY IMMEDIATE
        { $$ = makeString("initially_immediate"); }
    | NOT VALID
        { $$ = makeString("not_valid"); }
    | NO INHERIT
        { $$ = makeString("no_inherit"); }
;

opt_constraint_attr_spec:
      ConstraintAttributeSpec
    | /* empty */
        { $$ = {}; }
;

key_match:
      MATCH FULL
        { $$ = 1; }
    | MATCH PARTIAL
        { $$ = 2; }
    | MATCH SIMPLE
        { $$ = 3; }
    | /* empty */
        { $$ = 0; }
;

key_actions:
      key_action_def
        { $$ = $1; }
    | /* empty */
        { $$ = nullptr; }
;

key_action_def:
      ON DELETE_P key_action
        { $$ = nullptr; }
    | ON UPDATE key_action
        { $$ = nullptr; }
    | ON DELETE_P key_action ON UPDATE key_action
        { $$ = nullptr; }
    | ON UPDATE key_action ON DELETE_P key_action
        { $$ = nullptr; }
;

key_action:
      NO ACTION
    | RESTRICT
    | CASCADE
    | SET NULL_P
    | SET DEFAULT
;

opt_inherits:
      INHERITS '(' qualified_name_list ')'
        { $$ = std::move($3); }
    | /* empty */
        { $$ = {}; }
;

opt_column_list:
      '(' columnList ')'
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

columnList:
      columnList ',' columnElem
        { $1.push_back($3); $$ = std::move($1); }
    | columnElem
        { $$.push_back($1); }
;

columnElem:
      ColId
        { $$ = makeString($1); }
;

OptWith:
      WITH reloptions
        { $$ = std::move($2); }
    | WITH OIDS
        { $$ = {}; }
    | WITHOUT OIDS
        { $$ = {}; }
    | /* empty */
        { $$ = {}; }
;

opt_with:
      OptWith
    | /* empty */
        { $$ = {}; }
;

on_commit_opt:
      ON COMMIT DELETE_P ROWS
        { $$ = OnCommitAction::kDeleteRows; }
    | ON COMMIT DROP
        { $$ = OnCommitAction::kDrop; }
    | ON COMMIT PRESERVE ROWS
        { $$ = OnCommitAction::kPreserveRows; }
    | /* empty */
        { $$ = OnCommitAction::kNoop; }
;

opt_on_commit_opt:
      on_commit_opt
    | /* empty */
        { $$ = OnCommitAction::kNoop; }
;

OptTableSpace:
      TABLESPACE name
        { $$ = $2; }
    | /* empty */
        { $$ = ""; }
;

opt_table_access_method:
      USING access_method
        { $$ = $2; }
    | /* empty */
        { $$ = ""; }
;

access_method:
      ColId
        { $$ = $1; }
;

reloptions:
      '(' reloption_list ')'
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

opt_reloptions:
      reloptions
    | /* empty */
        { $$ = {}; }
;

reloption_list:
      reloption_elem
        { $$.push_back($1); }
    | reloption_list ',' reloption_elem
        { $1.push_back($3); $$ = std::move($1); }
;

reloption_elem:
      def_arg
        { $$ = $1; }
    | def_arg '=' def_arg
        {
            DefElem* n = makeNode<DefElem>();
            if (auto* v = dynamic_cast<Value*>($1)) {
                n->defname = v->GetString();
            }
            n->arg = $3;
            $$ = n;
        }
    | def_arg '.' def_arg
        { $$ = $1; }
;

def_elem:
      ColLabel
        { $$ = makeDefElem($1, nullptr, @1); }
    | ColLabel '=' def_arg
        { $$ = makeDefElem($1, $3, @1); }
    | ColLabel '.' ColLabel '=' def_arg
        {
            DefElem* n = makeDefElem($3, $5, @1);
            n->defnamespace = $1;
            $$ = n;
        }
;

def_arg:
      func_type
        { $$ = $1; }
    | Sconst
        { $$ = makeStrConst($1, @1); }
    | SignedIconst
        { $$ = makeIntConst($1, @1); }
    | TRUE_P
        { $$ = makeBoolAConst(true, @1); }
    | FALSE_P
        { $$ = makeBoolAConst(false, @1); }
;

func_type:
      Typename
        { $$ = $1; }
;

// PARTITION BY.
opt_partition_spec:
      PartitionSpec
    | /* empty */
        { $$ = nullptr; }
;

PartitionSpec:
      PARTITION BY part_strategy '(' part_params ')'
        {
            PartitionSpec* n = makeNode<PartitionSpec>();
            n->strategy = $3;
            n->part_params = std::move($5);
            n->location = @1;
            $$ = n;
        }
;

part_strategy:
      ColId
        { $$ = std::move($1); }
;

part_params:
      part_elem
        { $$.push_back($1); }
    | part_params ',' part_elem
        { $1.push_back($3); $$ = std::move($1); }
;

part_elem:
      ColId opt_collate opt_opclass
        {
            PartitionElem* n = makeNode<PartitionElem>();
            n->name = $1;
            n->location = @1;
            $$ = n;
        }
    | func_expr_windowless opt_collate opt_opclass
        {
            PartitionElem* n = makeNode<PartitionElem>();
            n->expr = $1;
            n->location = @1;
            $$ = n;
        }
    | '(' a_expr ')' opt_collate opt_opclass
        {
            PartitionElem* n = makeNode<PartitionElem>();
            n->expr = $2;
            n->location = @1;
            $$ = n;
        }
;

opt_collate:
      COLLATE any_name
    | /* empty */
;

opt_opclass:
      any_name
    | /* empty */
;

// Index parameters.
index_params:
      index_elem
        { $$.push_back($1); }
    | index_params ',' index_elem
        { $1.push_back($3); $$ = std::move($1); }
;

index_elem:
      ColId opt_collate opt_opclass opt_asc_desc opt_nulls_order
        {
            IndexElem* n = makeNode<IndexElem>();
            n->name = $1;
            n->ordering = $4;
            n->nulls_ordering = $5;
            $$ = n;
        }
    | func_expr_windowless opt_collate opt_opclass opt_asc_desc opt_nulls_order
        {
            IndexElem* n = makeNode<IndexElem>();
            n->expr = $1;
            n->ordering = $4;
            n->nulls_ordering = $5;
            $$ = n;
        }
    | '(' a_expr ')' opt_collate opt_opclass opt_asc_desc opt_nulls_order
        {
            IndexElem* n = makeNode<IndexElem>();
            n->expr = $2;
            n->ordering = $6;
            n->nulls_ordering = $7;
            $$ = n;
        }
;

func_expr_windowless:
      func_application
    | func_expr_common_subexpr
;

// DROP statement.
DropStmt:
      DROP drop_type_name any_name_list opt_drop_behavior
        {
            DropStmt* n = makeNode<DropStmt>();
            n->remove_type = $2;
            n->objects = std::move($3);
            n->behavior = $4;
            $$ = n;
        }
    | DROP drop_type_name IF_P EXISTS any_name_list opt_drop_behavior
        {
            DropStmt* n = makeNode<DropStmt>();
            n->remove_type = $2;
            n->objects = std::move($5);
            n->behavior = $6;
            n->missing_ok = true;
            $$ = n;
        }
    | DROP drop_type_name CONCURRENTLY any_name_list opt_drop_behavior
        {
            DropStmt* n = makeNode<DropStmt>();
            n->remove_type = $2;
            n->objects = std::move($4);
            n->behavior = $5;
            n->concurrent = true;
            $$ = n;
        }
    | DROP drop_type_name IF_P EXISTS CONCURRENTLY any_name_list opt_drop_behavior
        {
            DropStmt* n = makeNode<DropStmt>();
            n->remove_type = $2;
            n->objects = std::move($6);
            n->behavior = $7;
            n->missing_ok = true;
            n->concurrent = true;
            $$ = n;
        }
;

drop_type_name:
      TABLE
        { $$ = ObjectType::kTable; }
    | SEQUENCE
        { $$ = ObjectType::kSequence; }
    | VIEW
        { $$ = ObjectType::kView; }
    | MATERIALIZED
        { $$ = ObjectType::kMatview; }
    | INDEX
        { $$ = ObjectType::kIndex; }
    | TYPE_P
        { $$ = ObjectType::kType; }
    | DOMAIN_P
        { $$ = ObjectType::kDomain; }
    | COLLATION
        { $$ = ObjectType::kCollation; }
    | CONVERSION_P
        { $$ = ObjectType::kConversion; }
    | SCHEMA
        { $$ = ObjectType::kSchema; }
    | EXTENSION
        { $$ = ObjectType::kExtension; }
    | FUNCTION
        { $$ = ObjectType::kFunction; }
    | PROCEDURE
        { $$ = ObjectType::kProcedure; }
    | ROUTINE
        { $$ = ObjectType::kRoutine; }
    | AGGREGATE
        { $$ = ObjectType::kAggregate; }
    | OPERATOR
        { $$ = ObjectType::kOperator; }
    | LANGUAGE
        { $$ = ObjectType::kLanguage; }
    | CAST
        { $$ = ObjectType::kCast; }
    | TRIGGER
        { $$ = ObjectType::kTrigger; }
    | EVENT TRIGGER
        { $$ = ObjectType::kEventTrigger; }
    | RULE
        { $$ = ObjectType::kRule; }
    | FOREIGN DATA_P WRAPPER
        { $$ = ObjectType::kFdw; }
    | SERVER
        { $$ = ObjectType::kForeignServer; }
;

opt_drop_behavior:
      CASCADE
        { $$ = DropBehavior::kCascade; }
    | RESTRICT
        { $$ = DropBehavior::kRestrict; }
    | /* empty */
        { $$ = DropBehavior::kRestrict; }
;

// ===========================================================================
// TransactionStmt — BEGIN / COMMIT / ROLLBACK / SAVEPOINT / etc.
// ===========================================================================

TransactionStmt:
      BEGIN_P opt_transaction opt_transaction_mode_list
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kBegin;
            n->options = std::move($3);
            $$ = n;
        }
    | START TRANSACTION opt_transaction_mode_list
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kStart;
            n->options = std::move($3);
            $$ = n;
        }
    | COMMIT opt_transaction opt_transaction_mode_list
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kCommit;
            n->options = std::move($3);
            $$ = n;
        }
    | ROLLBACK opt_transaction opt_transaction_mode_list
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kRollback;
            n->options = std::move($3);
            $$ = n;
        }
    | SAVEPOINT ColId
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kSavepoint;
            n->savepoint_name = $2;
            $$ = n;
        }
    | RELEASE SAVEPOINT ColId
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kRelease;
            n->savepoint_name = $3;
            $$ = n;
        }
    | RELEASE ColId
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kRelease;
            n->savepoint_name = $2;
            $$ = n;
        }
    | ROLLBACK opt_transaction TO SAVEPOINT ColId
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kRollbackTo;
            n->savepoint_name = $5;
            $$ = n;
        }
    | ROLLBACK opt_transaction TO ColId
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kRollbackTo;
            n->savepoint_name = $4;
            $$ = n;
        }
    | PREPARE TRANSACTION Sconst
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kPrepare;
            n->gid = $3;
            $$ = n;
        }
    | COMMIT PREPARED Sconst
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kCommitPrepared;
            n->gid = $3;
            $$ = n;
        }
    | ROLLBACK PREPARED Sconst
        {
            TransactionStmt* n = makeNode<TransactionStmt>();
            n->kind = TransactionStmt::Kind::kRollbackPrepared;
            n->gid = $3;
            $$ = n;
        }
;

opt_transaction:
      TRANSACTION
    | /* empty */
;

opt_transaction_mode_list:
      opt_transaction_mode_list opt_transaction_mode_item
        {
            $1.push_back($2);
            $$ = std::move($1);
        }
    | /* empty */
        { $$ = {}; }
;

opt_transaction_mode_item:
      ISOLATION LEVEL iso_level
        { $$ = makeDefElem("transaction_isolation", makeString($3), @1); }
    | READ ONLY
        { $$ = makeDefElem("transaction_read_only", makeIntConst(1, @1), @1); }
    | READ WRITE
        { $$ = makeDefElem("transaction_read_only", makeIntConst(0, @1), @1); }
    | DEFERRABLE
        { $$ = makeDefElem("transaction_deferrable", makeIntConst(1, @1), @1); }
    | NOT DEFERRABLE
        { $$ = makeDefElem("transaction_deferrable", makeIntConst(0, @1), @1); }
;

iso_level:
      READ UNCOMMITTED
        { $$ = "read uncommitted"; }
    | READ COMMITTED
        { $$ = "read committed"; }
    | REPEATABLE READ
        { $$ = "repeatable read"; }
    | SERIALIZABLE
        { $$ = "serializable"; }
;

// ===========================================================================
// TruncateStmt — TRUNCATE TABLE
// ===========================================================================

TruncateStmt:
      TRUNCATE opt_table relation_expr_list opt_restart_seqs opt_drop_behavior
        {
            TruncateStmt* n = makeNode<TruncateStmt>();
            n->relations = std::move($3);
            n->restart_seqs = $4;
            n->behavior = $5;
            $$ = n;
        }
;

opt_table:
      TABLE
    | /* empty */
;

opt_restart_seqs:
      RESTART IDENTITY_P
        { $$ = true; }
    | CONTINUE_P IDENTITY_P
        { $$ = false; }
    | /* empty */
        { $$ = false; }
;

relation_expr_list:
      relation_expr
        { $$.push_back($1); }
    | relation_expr_list ',' relation_expr
        { $1.push_back($3); $$ = std::move($1); }
;

// ===========================================================================
// ExplainStmt — EXPLAIN
// ===========================================================================

ExplainStmt:
      EXPLAIN opt_explain_stmt
        {
            ExplainStmt* n = makeNode<ExplainStmt>();
            n->query = $2;
            $$ = n;
        }
    | EXPLAIN analyze_options opt_explain_stmt
        {
            ExplainStmt* n = makeNode<ExplainStmt>();
            n->query = $3;
            n->options = std::move($2);
            $$ = n;
        }
;

opt_explain_stmt:
      SelectStmt
    | InsertStmt
    | UpdateStmt
    | DeleteStmt
    | CreateAsStmt
;

analyze_options:
      analyze_option
        { $$.push_back($1); }
    | analyze_options analyze_option
        { $1.push_back($2); $$ = std::move($1); }
;

analyze_option:
      ANALYZE
        { $$ = makeDefElem("analyze", nullptr, @1); }
    | VERBOSE
        { $$ = makeDefElem("verbose", nullptr, @1); }
    | COSTS
        { $$ = makeDefElem("costs", nullptr, @1); }
    | SETTINGS
        { $$ = makeDefElem("settings", nullptr, @1); }
    | BUFFERS
        { $$ = makeDefElem("buffers", nullptr, @1); }
    | WAL
        { $$ = makeDefElem("wal", nullptr, @1); }
    | TIMING
        { $$ = makeDefElem("timing", nullptr, @1); }
    | SUMMARY
        { $$ = makeDefElem("summary", nullptr, @1); }
    | FORMAT TEXT_P
        { $$ = makeDefElem("format", makeString("text"), @1); }
    | FORMAT XML_P
        { $$ = makeDefElem("format", makeString("xml"), @1); }
    | FORMAT JSON
        { $$ = makeDefElem("format", makeString("json"), @1); }
    | FORMAT YAML
        { $$ = makeDefElem("format", makeString("yaml"), @1); }
;

// ===========================================================================
// CommentStmt — COMMENT ON
// ===========================================================================

CommentStmt:
      COMMENT ON comment_type_any_name any_name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = $3;
            n->object = $4;
            n->comment = $6;
            $$ = n;
        }
    | COMMENT ON COLUMN ColId '.' attr_name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = ObjectType::kColumn;
            n->object.push_back(makeString($4));
            n->object.push_back(makeString($6));
            n->comment = $8;
            $$ = n;
        }
    | COMMENT ON TABLE name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = ObjectType::kTable;
            n->object.push_back(makeString($4));
            n->comment = $6;
            $$ = n;
        }
    | COMMENT ON DATABASE name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = ObjectType::kDatabase;
            n->object.push_back(makeString($4));
            n->comment = $6;
            $$ = n;
        }
    | COMMENT ON SCHEMA name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = ObjectType::kSchema;
            n->object.push_back(makeString($4));
            n->comment = $6;
            $$ = n;
        }
    | COMMENT ON INDEX name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = ObjectType::kIndex;
            n->object.push_back(makeString($4));
            n->comment = $6;
            $$ = n;
        }
    | COMMENT ON VIEW name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = ObjectType::kView;
            n->object.push_back(makeString($4));
            n->comment = $6;
            $$ = n;
        }
    | COMMENT ON SEQUENCE name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = ObjectType::kSequence;
            n->object.push_back(makeString($4));
            n->comment = $6;
            $$ = n;
        }
    | COMMENT ON FUNCTION function_name IS comment_text
        {
            CommentStmt* n = makeNode<CommentStmt>();
            n->objtype = ObjectType::kFunction;
            n->object = $4;
            n->comment = $6;
            $$ = n;
        }
;

comment_type_any_name:
      TABLE
        { $$ = ObjectType::kTable; }
    | INDEX
        { $$ = ObjectType::kIndex; }
    | VIEW
        { $$ = ObjectType::kView; }
    | SEQUENCE
        { $$ = ObjectType::kSequence; }
    | SCHEMA
        { $$ = ObjectType::kSchema; }
    | TYPE_P
        { $$ = ObjectType::kType; }
    | DOMAIN_P
        { $$ = ObjectType::kDomain; }
    | COLLATION
        { $$ = ObjectType::kCollation; }
    | CONVERSION_P
        { $$ = ObjectType::kConversion; }
;

comment_text:
      Sconst
        { $$ = $1; }
    | NULL_P
        { $$ = ""; }
;

function_name:
      ColId
        {
            $$.push_back(makeString($1));
        }
    | ColId '.' ColId
        {
            $$.push_back(makeString($1));
            $$.push_back(makeString($3));
        }
;

// ===========================================================================
// IndexStmt — CREATE INDEX
// ===========================================================================

IndexStmt:
      CREATE opt_unique INDEX opt_concurrently opt_index_name
          ON qualified_name access_method_clause '(' index_params ')'
          opt_include opt_reloptions OptTableSpace opt_where_clause
        {
            IndexStmt* n = makeNode<IndexStmt>();
            n->unique = $2;
            n->concurrent = $4;
            n->idxname = $5;
            n->relation = static_cast<RangeVar*>($7);
            n->access_method = $8;
            n->index_params = std::move($10);
            (void)$12;
            n->options = std::move($13);
            (void)$14;
            if ($15 != nullptr) n->where_clause.push_back($15);
            $$ = n;
        }
    | CREATE opt_unique INDEX opt_concurrently IF_P NOT EXISTS name
          ON qualified_name access_method_clause '(' index_params ')'
          opt_include opt_reloptions OptTableSpace opt_where_clause
        {
            IndexStmt* n = makeNode<IndexStmt>();
            n->unique = $2;
            n->concurrent = $4;
            n->idxname = $8;
            n->relation = static_cast<RangeVar*>($10);
            n->access_method = $11;
            n->index_params = std::move($13);
            (void)$15;
            n->options = std::move($16);
            (void)$17;
            if ($18 != nullptr) n->where_clause.push_back($18);
            n->if_not_exists = true;
            $$ = n;
        }
;

opt_unique:
      UNIQUE
        { $$ = true; }
    | /* empty */
        { $$ = false; }
;

opt_concurrently:
      CONCURRENTLY
        { $$ = true; }
    | /* empty */
        { $$ = false; }
;

opt_index_name:
      name
        { $$ = $1; }
    | /* empty */
        { $$ = ""; }
;

access_method_clause:
      USING access_method
        { $$ = $2; }
    | /* empty */
        { $$ = "btree"; }
;

opt_include:
      INCLUDE '(' index_including_params ')'
        { (void)$3; }
    | /* empty */
        { }
;

index_including_params:
      index_elem
        { $$.push_back($1); }
    | index_including_params ',' index_elem
        { $1.push_back($3); $$ = std::move($1); }
;

opt_reloptions:
      WITH reloptions
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

// ===========================================================================
// ViewStmt — CREATE VIEW
// ===========================================================================

ViewStmt:
      CREATE opt_or_replace VIEW qualified_name opt_column_list opt_reloptions
          AS SelectStmt opt_check_option
        {
            ViewStmt* n = makeNode<ViewStmt>();
            n->replace = $2;
            n->view = static_cast<RangeVar*>($4);
            n->aliases = std::move($5);
            n->options = std::move($6);
            n->query = $8;
            (void)$9;
            $$ = n;
        }
;

opt_or_replace:
      OR REPLACE
        { $$ = true; }
    | /* empty */
        { $$ = false; }
;

opt_column_list:
      '(' columnList ')'
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

columnList:
      columnElem
        { $$.push_back($1); }
    | columnList ',' columnElem
        { $1.push_back($3); $$ = std::move($1); }
;

columnElem:
      ColId
        { $$ = makeString($1); }
;

opt_check_option:
      WITH CHECK OPTION
        { $$ = 0; }
    | WITH CASCADED CHECK OPTION
        { $$ = 0; }
    | WITH LOCAL CHECK OPTION
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

// ===========================================================================
// CreateAsStmt — CREATE TABLE AS SELECT / SELECT INTO
// ===========================================================================

CreateAsStmt:
      CREATE OptTemp TABLE create_as_target AS SelectStmt opt_with_data
        {
            CreateAsStmt* n = makeNode<CreateAsStmt>();
            n->into = static_cast<IntoClause*>($4);
            n->query = $6;
            (void)$7;
            $$ = n;
        }
    | CREATE OptTemp TABLE IF_P NOT EXISTS create_as_target AS SelectStmt opt_with_data
        {
            CreateAsStmt* n = makeNode<CreateAsStmt>();
            n->into = static_cast<IntoClause*>($7);
            n->query = $9;
            (void)$10;
            n->if_not_exists = true;
            $$ = n;
        }
;

create_as_target:
      qualified_name opt_column_list opt_with opt_on_commit_opt OptTableSpace
        {
            IntoClause* n = makeNode<IntoClause>();
            n->rel = static_cast<RangeVar*>($1);
            n->col_names = std::move($2);
            (void)$3;
            n->on_commit = $4;
            n->table_space_name = $5;
            $$ = n;
        }
;

opt_with_data:
      WITH DATA_P
        { $$ = true; }
    | WITH NO DATA_P
        { $$ = false; }
    | /* empty */
        { $$ = true; }
;

// ===========================================================================
// CreateSchemaStmt — CREATE SCHEMA
// ===========================================================================

CreateSchemaStmt:
      CREATE SCHEMA opt_schema_name_clause AUTHORIZATION RoleSpec opt_schema_elements
        {
            CreateSchemaStmt* n = makeNode<CreateSchemaStmt>();
            n->schemaname = $3;
            n->authrole = static_cast<RoleSpec*>($5);
            n->schema_elts = std::move($6);
            $$ = n;
        }
    | CREATE SCHEMA ColId opt_schema_elements
        {
            CreateSchemaStmt* n = makeNode<CreateSchemaStmt>();
            n->schemaname = $3;
            n->schema_elts = std::move($4);
            $$ = n;
        }
    | CREATE SCHEMA IF_P NOT EXISTS opt_schema_name_clause AUTHORIZATION RoleSpec opt_schema_elements
        {
            CreateSchemaStmt* n = makeNode<CreateSchemaStmt>();
            n->schemaname = $6;
            n->authrole = static_cast<RoleSpec*>($8);
            n->schema_elts = std::move($9);
            n->if_not_exists = true;
            $$ = n;
        }
    | CREATE SCHEMA IF_P NOT EXISTS ColId opt_schema_elements
        {
            CreateSchemaStmt* n = makeNode<CreateSchemaStmt>();
            n->schemaname = $6;
            n->schema_elts = std::move($7);
            n->if_not_exists = true;
            $$ = n;
        }
;

opt_schema_name_clause:
      ColId
        { $$ = $1; }
    | /* empty */
        { $$ = ""; }
;

opt_schema_elements:
      schema_stmts
        { $$ = std::move($1); }
    | /* empty */
        { $$ = {}; }
;

schema_stmts:
      schema_stmt
        { $$.push_back($1); }
    | schema_stmts schema_stmt
        { $1.push_back($2); $$ = std::move($1); }
;

schema_stmt:
      CreateStmt
    | IndexStmt
    | CreateSeqStmt
    | CreateTrigStmt
    | GrantStmt
    | ViewStmt
;

// ===========================================================================
// AlterTableStmt — ALTER TABLE
// ===========================================================================

AlterTableStmt:
      ALTER TABLE relation_expr alter_table_cmds
        {
            AlterTableStmt* n = makeNode<AlterTableStmt>();
            n->relation = static_cast<RangeVar*>($3);
            n->cmds = std::move($4);
            n->objtype = ObjectType::kTable;
            $$ = n;
        }
    | ALTER TABLE IF_P EXISTS relation_expr alter_table_cmds
        {
            AlterTableStmt* n = makeNode<AlterTableStmt>();
            n->relation = static_cast<RangeVar*>($5);
            n->cmds = std::move($6);
            n->objtype = ObjectType::kTable;
            n->missing_ok = true;
            $$ = n;
        }
    | ALTER INDEX qualified_name alter_table_cmds
        {
            AlterTableStmt* n = makeNode<AlterTableStmt>();
            n->relation = static_cast<RangeVar*>($3);
            n->cmds = std::move($4);
            n->objtype = ObjectType::kIndex;
            $$ = n;
        }
    | ALTER VIEW qualified_name alter_table_cmds
        {
            AlterTableStmt* n = makeNode<AlterTableStmt>();
            n->relation = static_cast<RangeVar*>($3);
            n->cmds = std::move($4);
            n->objtype = ObjectType::kView;
            $$ = n;
        }
    | ALTER SEQUENCE qualified_name alter_table_cmds
        {
            AlterTableStmt* n = makeNode<AlterTableStmt>();
            n->relation = static_cast<RangeVar*>($3);
            n->cmds = std::move($4);
            n->objtype = ObjectType::kSequence;
            $$ = n;
        }
;

alter_table_cmds:
      alter_table_cmd
        { $$.push_back($1); }
    | alter_table_cmds ',' alter_table_cmd
        { $1.push_back($3); $$ = std::move($1); }
;

alter_table_cmd:
      ADD_P opt_column columnDef
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kAddColumn;
            n->def = $3;
            $$ = n;
        }
    | ADD_P opt_column IF_P NOT EXISTS columnDef
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kAddColumnRecurse;
            n->def = $6;
            n->missing_ok = true;
            $$ = n;
        }
    | DROP opt_column IF_P EXISTS ColId opt_drop_behavior
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kDropColumnRecurse;
            n->name = $5;
            n->behavior = $6;
            n->missing_ok = true;
            $$ = n;
        }
    | DROP opt_column ColId opt_drop_behavior
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kDropColumn;
            n->name = $3;
            n->behavior = $4;
            $$ = n;
        }
    | ALTER opt_column ColId opt_set_data TYPE_P Typename opt_collate_clause opt_alter_column_action
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kAlterColumnType;
            n->name = $3;
            n->def = $6;
            (void)$7;
            (void)$8;
            $$ = n;
        }
    | ALTER opt_column ColId SET DEFAULT a_expr
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kColumnDefault;
            n->name = $3;
            n->def = $6;
            $$ = n;
        }
    | ALTER opt_column ColId DROP DEFAULT
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kColumnDefault;
            n->name = $3;
            $$ = n;
        }
    | ALTER opt_column ColId SET NOT NULL_P
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kSetNotNull;
            n->name = $3;
            $$ = n;
        }
    | ALTER opt_column ColId DROP NOT NULL_P
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kDropNotNull;
            n->name = $3;
            $$ = n;
        }
    | ALTER opt_column ColId SET STATISTICS SignedIconst
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kSetStatistics;
            n->name = $3;
            n->num = $6;
            $$ = n;
        }
    | SET TABLESPACE name
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kSetTableSpace;
            n->name = $3;
            $$ = n;
        }
    | OWNER TO RoleSpec
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kChangeOwner;
            n->newowner = static_cast<RoleSpec*>($3);
            $$ = n;
        }
    | CLUSTER ON name
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kClusterOn;
            n->name = $3;
            $$ = n;
        }
    | SET WITHOUT OIDS
        {
            AlterTableCmd* n = makeNode<AlterTableCmd>();
            n->subtype = AlterTableType::kDropOids;
            $$ = n;
        }
;

opt_column:
      COLUMN
    | /* empty */
;

opt_set_data:
      SET DATA_P
    | /* empty */
;

opt_collate_clause:
      COLLATE any_name
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

opt_alter_column_action:
      SET ATTRIBUTE ColId
        { $$ = 0; }
    | DROP ATTRIBUTE ColId
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

SignedIconst:
      Iconst
        { $$ = $1; }
    | '-' Iconst
        { $$ = -$2; }
;

// ===========================================================================
// VacuumStmt — VACUUM / ANALYZE
// ===========================================================================

VacuumStmt:
      VACUUM opt_full opt_freeze opt_verbose opt_analyze_options
        {
            VacuumStmt* n = makeNode<VacuumStmt>();
            n->is_vacuumcmd = true;
            n->full = ($2 != 0);
            n->freeze = ($3 != 0);
            (void)$4;
            n->options = std::move($5);
            $$ = n;
        }
    | VACUUM opt_full opt_freeze opt_verbose opt_analyze_options vacuum_relation_list
        {
            VacuumStmt* n = makeNode<VacuumStmt>();
            n->is_vacuumcmd = true;
            n->full = ($2 != 0);
            n->freeze = ($3 != 0);
            (void)$4;
            n->options = std::move($5);
            n->rels = std::move($6);
            $$ = n;
        }
    | ANALYZE opt_verbose opt_analyze_options
        {
            VacuumStmt* n = makeNode<VacuumStmt>();
            n->is_vacuumcmd = false;
            (void)$2;
            n->options = std::move($3);
            $$ = n;
        }
    | ANALYZE opt_verbose opt_analyze_options vacuum_relation_list
        {
            VacuumStmt* n = makeNode<VacuumStmt>();
            n->is_vacuumcmd = false;
            (void)$2;
            n->options = std::move($3);
            n->rels = std::move($4);
            $$ = n;
        }
;

opt_full:
      FULL
        { $$ = 1; }
    | /* empty */
        { $$ = 0; }
;

opt_freeze:
      FREEZE
        { $$ = 1; }
    | /* empty */
        { $$ = 0; }
;

opt_verbose:
      VERBOSE
        { $$ = true; }
    | /* empty */
        { $$ = false; }
;

opt_analyze_options:
      analyze_options
        { $$ = std::move($1); }
    | /* empty */
        { $$ = {}; }
;

vacuum_relation_list:
      vacuum_relation
        { $$.push_back($1); }
    | vacuum_relation_list ',' vacuum_relation
        { $1.push_back($3); $$ = std::move($1); }
;

vacuum_relation:
      qualified_name opt_name_list
        {
            RangeVar* rv = static_cast<RangeVar*>($1);
            (void)rv;
            $$ = $1;
        }
;

opt_name_list:
      '(' name_list ')'
        { }
    | /* empty */
        { }
;

name_list:
      name
        { $$.push_back(makeString($1)); }
    | name_list ',' name
        { $1.push_back(makeString($3)); $$ = std::move($1); }
;

// ===========================================================================
// VariableSetStmt — SET / RESET
// ===========================================================================

VariableSetStmt:
      SET set_rest
        {
            VariableSetStmt* n = static_cast<VariableSetStmt*>($2);
            n->is_local = false;
            $$ = n;
        }
    | SET LOCAL set_rest
        {
            VariableSetStmt* n = static_cast<VariableSetStmt*>($3);
            n->is_local = true;
            $$ = n;
        }
    | SET SESSION set_rest
        {
            VariableSetStmt* n = static_cast<VariableSetStmt*>($3);
            n->is_local = false;
            $$ = n;
        }
;

VariableResetStmt:
      RESET ColId
        {
            VariableSetStmt* n = makeNode<VariableSetStmt>();
            n->kind = VariableSetStmt::Kind::kReset;
            n->name = $2;
            $$ = n;
        }
    | RESET ALL
        {
            VariableSetStmt* n = makeNode<VariableSetStmt>();
            n->kind = VariableSetStmt::Kind::kResetAll;
            $$ = n;
        }
;

set_rest:
      ColId TO var_list
        {
            VariableSetStmt* n = makeNode<VariableSetStmt>();
            n->kind = VariableSetStmt::Kind::kSet;
            n->name = $1;
            n->args = std::move($3);
            $$ = n;
        }
    | ColId '=' var_list
        {
            VariableSetStmt* n = makeNode<VariableSetStmt>();
            n->kind = VariableSetStmt::Kind::kSet;
            n->name = $1;
            n->args = std::move($3);
            $$ = n;
        }
    | ColId TO DEFAULT
        {
            VariableSetStmt* n = makeNode<VariableSetStmt>();
            n->kind = VariableSetStmt::Kind::kSet;
            n->name = $1;
            $$ = n;
        }
    | ColId '=' DEFAULT
        {
            VariableSetStmt* n = makeNode<VariableSetStmt>();
            n->kind = VariableSetStmt::Kind::kSet;
            n->name = $1;
            $$ = n;
        }
    | TIME ZONE zone_value
        {
            VariableSetStmt* n = makeNode<VariableSetStmt>();
            n->kind = VariableSetStmt::Kind::kSet;
            n->name = "timezone";
            n->args.push_back($3);
            $$ = n;
        }
;

var_list:
      var_value
        { $$.push_back($1); }
    | var_list ',' var_value
        { $1.push_back($3); $$ = std::move($1); }
;

var_value:
      a_expr
        { $$ = $1; }
    | ON
        { $$ = makeStrConst("on", @1); }
    | OFF
        { $$ = makeStrConst("off", @1); }
;

zone_value:
      Sconst
        { $$ = makeStrConst($1, @1); }
    | IDENT
        { $$ = makeStrConst($1, @1); }
    | interval_qualifier
        { $$ = $1; }
    | DEFAULT
        { $$ = nullptr; }
    | LOCAL
        { $$ = nullptr; }
;

interval_qualifier:
      INTERVAL Sconst
        { $$ = makeStrConst($2, @1); }
;

// ===========================================================================
// ClusterStmt — CLUSTER
// ===========================================================================

ClusterStmt:
      CLUSTER opt_verbose qualified_name opt_cluster_index
        {
            ClusterStmt* n = makeNode<ClusterStmt>();
            n->verbose = $2;
            n->relation = static_cast<RangeVar*>($3);
            n->indexname = $4;
            $$ = n;
        }
    | CLUSTER opt_verbose
        {
            ClusterStmt* n = makeNode<ClusterStmt>();
            n->verbose = $2;
            $$ = n;
        }
;

opt_cluster_index:
      USING name
        { $$ = $2; }
    | /* empty */
        { $$ = ""; }
;

// ===========================================================================
// LockStmt — LOCK TABLE
// ===========================================================================

LockStmt:
      LOCK_P opt_table relation_expr_list opt_lock opt_nowait
        {
            LockStmt* n = makeNode<LockStmt>();
            n->relations = std::move($3);
            n->mode = $4;
            n->nowait = $5;
            $$ = n;
        }
;

opt_lock:
      IN_P lock_type MODE
        { $$ = $2; }
    | /* empty */
        { $$ = 4; }  // ACCESS EXCLUSIVE by default
;

lock_type:
      ACCESS SHARE
        { $$ = 1; }
    | ROW SHARE
        { $$ = 2; }
    | ROW EXCLUSIVE
        { $$ = 3; }
    | SHARE UPDATE EXCLUSIVE
        { $$ = 4; }
    | SHARE
        { $$ = 5; }
    | SHARE ROW EXCLUSIVE
        { $$ = 6; }
    | EXCLUSIVE
        { $$ = 7; }
    | ACCESS EXCLUSIVE
        { $$ = 8; }
;

opt_nowait:
      NOWAIT
        { $$ = true; }
    | /* empty */
        { $$ = false; }
;

// ===========================================================================
// DiscardStmt — DISCARD
// ===========================================================================

DiscardStmt:
      DISCARD ALL
        {
            DiscardStmt* n = makeNode<DiscardStmt>();
            n->target = DiscardStmt::Target::kAll;
            $$ = n;
        }
    | DISCARD TEMP
        {
            DiscardStmt* n = makeNode<DiscardStmt>();
            n->target = DiscardStmt::Target::kTemp;
            $$ = n;
        }
    | DISCARD TEMPORARY
        {
            DiscardStmt* n = makeNode<DiscardStmt>();
            n->target = DiscardStmt::Target::kTemp;
            $$ = n;
        }
    | DISCARD SEQUENCES
        {
            DiscardStmt* n = makeNode<DiscardStmt>();
            n->target = DiscardStmt::Target::kSequences;
            $$ = n;
        }
;

// ===========================================================================
// NotifyStmt / ListenStmt / UnlistenStmt
// ===========================================================================

NotifyStmt:
      NOTIFY ColId opt_notify_payload
        {
            NotifyStmt* n = makeNode<NotifyStmt>();
            n->conditionname = $2;
            n->payload = $3;
            $$ = n;
        }
;

opt_notify_payload:
      ',' Sconst
        { $$ = $2; }
    | /* empty */
        { $$ = ""; }
;

ListenStmt:
      LISTEN ColId
        {
            ListenStmt* n = makeNode<ListenStmt>();
            n->conditionname = $2;
            $$ = n;
        }
;

UnlistenStmt:
      UNLISTEN ColId
        {
            UnlistenStmt* n = makeNode<UnlistenStmt>();
            n->conditionname = $2;
            $$ = n;
        }
    | UNLISTEN '*'
        {
            UnlistenStmt* n = makeNode<UnlistenStmt>();
            $$ = n;
        }
;

// ===========================================================================
// CheckPointStmt — CHECKPOINT
// ===========================================================================

CheckPointStmt:
      CHECKPOINT
        {
            CheckPointStmt* n = makeNode<CheckPointStmt>();
            $$ = n;
        }
;

// ===========================================================================
// ReindexStmt — REINDEX
// ===========================================================================

ReindexStmt:
      REINDEX opt_reindex_option reindex_target_relation opt_progress
        {
            ReindexStmt* n = makeNode<ReindexStmt>();
            n->kind = $3.first;
            n->name = $3.second;
            n->concurrently = $2;
            (void)$4;
            $$ = n;
        }
    | REINDEX opt_reindex_option SCHEMA name opt_progress
        {
            ReindexStmt* n = makeNode<ReindexStmt>();
            n->kind = ReindexStmt::Kind::kSchema;
            n->name = $4;
            n->concurrently = $2;
            (void)$5;
            $$ = n;
        }
    | REINDEX opt_reindex_option SYSTEM_P name opt_progress
        {
            ReindexStmt* n = makeNode<ReindexStmt>();
            n->kind = ReindexStmt::Kind::kSystem;
            n->name = $4;
            n->concurrently = $2;
            (void)$5;
            $$ = n;
        }
    | REINDEX opt_reindex_option DATABASE name opt_progress
        {
            ReindexStmt* n = makeNode<ReindexStmt>();
            n->kind = ReindexStmt::Kind::kDatabase;
            n->name = $4;
            n->concurrently = $2;
            (void)$5;
            $$ = n;
        }
;

opt_reindex_option:
      '(' reindex_list ')'
        { $$ = false; }
    | CONCURRENTLY
        { $$ = true; }
    | /* empty */
        { $$ = false; }
;

reindex_list:
      reindex_option
    | reindex_list ',' reindex_option
;

reindex_option:
      VERBOSE
    | TABLESPACE name
;

reindex_target_relation:
      INDEX qualified_name
        {
            $$.first = ReindexStmt::Kind::kIndex;
            $$.second = "";
            (void)$2;
        }
    | TABLE qualified_name
        {
            $$.first = ReindexStmt::Kind::kTable;
            $$.second = "";
            (void)$2;
        }
;

opt_progress:
      '(' reindex_list ')'
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

// ===========================================================================
// DeallocateStmt — DEALLOCATE
// ===========================================================================

DeallocateStmt:
      DEALLOCATE name
        {
            DeallocateStmt* n = makeNode<DeallocateStmt>();
            n->name = $2;
            $$ = n;
        }
    | DEALLOCATE PREPARE name
        {
            DeallocateStmt* n = makeNode<DeallocateStmt>();
            n->name = $3;
            $$ = n;
        }
    | DEALLOCATE ALL
        {
            DeallocateStmt* n = makeNode<DeallocateStmt>();
            $$ = n;
        }
    | DEALLOCATE PREPARE ALL
        {
            DeallocateStmt* n = makeNode<DeallocateStmt>();
            $$ = n;
        }
;

// ===========================================================================
// PrepareStmt / ExecuteStmt
// ===========================================================================

PrepareStmt:
      PREPARE name opt_preparable_type AS SelectStmt
        {
            PrepareStmt* n = makeNode<PrepareStmt>();
            n->name = $2;
            n->argtypes = std::move($3);
            n->query = $5;
            $$ = n;
        }
    | PREPARE name opt_preparable_type AS InsertStmt
        {
            PrepareStmt* n = makeNode<PrepareStmt>();
            n->name = $2;
            n->argtypes = std::move($3);
            n->query = $5;
            $$ = n;
        }
    | PREPARE name opt_preparable_type AS UpdateStmt
        {
            PrepareStmt* n = makeNode<PrepareStmt>();
            n->name = $2;
            n->argtypes = std::move($3);
            n->query = $5;
            $$ = n;
        }
    | PREPARE name opt_preparable_type AS DeleteStmt
        {
            PrepareStmt* n = makeNode<PrepareStmt>();
            n->name = $2;
            n->argtypes = std::move($3);
            n->query = $5;
            $$ = n;
        }
;

opt_preparable_type:
      '(' preparable_type_list ')'
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

preparable_type_list:
      Typename
        { $$.push_back($1); }
    | preparable_type_list ',' Typename
        { $1.push_back($3); $$ = std::move($1); }
;

ExecuteStmt:
      EXECUTE name opt_execute_param_clause
        {
            ExecuteStmt* n = makeNode<ExecuteStmt>();
            n->name = $2;
            n->params = std::move($3);
            $$ = n;
        }
;

opt_execute_param_clause:
      '(' expr_list ')'
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

// ===========================================================================
// LoadStmt — LOAD
// ===========================================================================

LoadStmt:
      LOAD file_name
        {
            LoadStmt* n = makeNode<LoadStmt>();
            n->filename = $2;
            $$ = n;
        }
;

file_name:
      Sconst
        { $$ = $1; }
;

// ===========================================================================
// CallStmt — CALL
// ===========================================================================

CallStmt:
      CALL func_application
        {
            CallStmt* n = makeNode<CallStmt>();
            n->funccall = $2;
            $$ = n;
        }
;

// ===========================================================================
// RenameStmt — ALTER ... RENAME TO
// ===========================================================================

RenameStmt:
      ALTER TABLE relation_expr RENAME TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kTable;
            n->relation_type = ObjectType::kTable;
            n->relation = static_cast<RangeVar*>($3);
            n->newname = $6;
            $$ = n;
        }
    | ALTER TABLE IF_P EXISTS relation_expr RENAME TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kTable;
            n->relation_type = ObjectType::kTable;
            n->relation = static_cast<RangeVar*>($5);
            n->newname = $8;
            n->missing_ok = true;
            $$ = n;
        }
    | ALTER TABLE relation_expr RENAME opt_column ColId TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kColumn;
            n->relation_type = ObjectType::kTable;
            n->relation = static_cast<RangeVar*>($3);
            n->subname = $6;
            n->newname = $8;
            $$ = n;
        }
    | ALTER TABLE relation_expr RENAME CONSTRAINT name TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kTabconstraint;
            n->relation_type = ObjectType::kTable;
            n->relation = static_cast<RangeVar*>($3);
            n->subname = $6;
            n->newname = $8;
            $$ = n;
        }
    | ALTER VIEW qualified_name RENAME TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kView;
            n->relation_type = ObjectType::kView;
            n->relation = static_cast<RangeVar*>($3);
            n->newname = $6;
            $$ = n;
        }
    | ALTER INDEX qualified_name RENAME TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kIndex;
            n->relation_type = ObjectType::kIndex;
            n->relation = static_cast<RangeVar*>($3);
            n->newname = $6;
            $$ = n;
        }
    | ALTER SEQUENCE qualified_name RENAME TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kSequence;
            n->relation_type = ObjectType::kSequence;
            n->relation = static_cast<RangeVar*>($3);
            n->newname = $6;
            $$ = n;
        }
    | ALTER SCHEMA name RENAME TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kSchema;
            n->object.push_back(makeString($3));
            n->newname = $6;
            $$ = n;
        }
    | ALTER DATABASE name RENAME TO name
        {
            RenameStmt* n = makeNode<RenameStmt>();
            n->rename_type = ObjectType::kDatabase;
            n->object.push_back(makeString($3));
            n->newname = $6;
            $$ = n;
        }
;

// ===========================================================================
// AlterOwnerStmt — ALTER ... OWNER TO
// ===========================================================================

AlterOwnerStmt:
      ALTER TABLE relation_expr OWNER TO RoleSpec
        {
            AlterOwnerStmt* n = makeNode<AlterOwnerStmt>();
            n->object_type = ObjectType::kTable;
            n->object.push_back($3);
            n->newowner = static_cast<RoleSpec*>($6);
            $$ = n;
        }
    | ALTER VIEW qualified_name OWNER TO RoleSpec
        {
            AlterOwnerStmt* n = makeNode<AlterOwnerStmt>();
            n->object_type = ObjectType::kView;
            n->object.push_back($3);
            n->newowner = static_cast<RoleSpec*>($6);
            $$ = n;
        }
    | ALTER SCHEMA name OWNER TO RoleSpec
        {
            AlterOwnerStmt* n = makeNode<AlterOwnerStmt>();
            n->object_type = ObjectType::kSchema;
            n->object.push_back(makeString($3));
            n->newowner = static_cast<RoleSpec*>($6);
            $$ = n;
        }
    | ALTER DATABASE name OWNER TO RoleSpec
        {
            AlterOwnerStmt* n = makeNode<AlterOwnerStmt>();
            n->object_type = ObjectType::kDatabase;
            n->object.push_back(makeString($3));
            n->newowner = static_cast<RoleSpec*>($6);
            $$ = n;
        }
;

// ===========================================================================
// CreateSeqStmt / AlterSeqStmt — CREATE SEQUENCE / ALTER SEQUENCE
// ===========================================================================

CreateSeqStmt:
      CREATE opt_temp SEQUENCE qualified_name OptSeqOptList
        {
            CreateSeqStmt* n = makeNode<CreateSeqStmt>();
            n->sequence = static_cast<RangeVar*>($4);
            n->options = std::move($5);
            $$ = n;
        }
    | CREATE opt_temp SEQUENCE IF_P NOT EXISTS qualified_name OptSeqOptList
        {
            CreateSeqStmt* n = makeNode<CreateSeqStmt>();
            n->sequence = static_cast<RangeVar*>($7);
            n->options = std::move($8);
            n->if_not_exists = true;
            $$ = n;
        }
;

opt_temp:
      TEMPORARY
        { $$ = 1; }
    | TEMP
        { $$ = 1; }
    | /* empty */
        { $$ = 0; }
;

OptSeqOptList:
      SeqOptList
        { $$ = std::move($1); }
    | /* empty */
        { $$ = {}; }
;

SeqOptList:
      SeqOptElem
        { $$.push_back($1); }
    | SeqOptList SeqOptElem
        { $1.push_back($2); $$ = std::move($1); }
;

SeqOptElem:
      AS Typename
        { $$ = makeDefElem("as", $2, @1); }
    | CACHE Iconst
        { $$ = makeDefElem("cache", makeIntConst($2, @1), @1); }
    | CYCLE
        { $$ = makeDefElem("cycle", nullptr, @1); }
    | NO CYCLE
        { $$ = makeDefElem("cycle", makeIntConst(0, @1), @1); }
    | INCREMENT opt_by Iconst
        { $$ = makeDefElem("increment", makeIntConst($3, @1), @1); }
    | INCREMENT Iconst
        { $$ = makeDefElem("increment", makeIntConst($2, @1), @1); }
    | MINVALUE opt_by Iconst
        { $$ = makeDefElem("minvalue", makeIntConst($3, @1), @1); }
    | MINVALUE Iconst
        { $$ = makeDefElem("minvalue", makeIntConst($2, @1), @1); }
    | NO MINVALUE
        { $$ = makeDefElem("minvalue", nullptr, @1); }
    | MAXVALUE opt_by Iconst
        { $$ = makeDefElem("maxvalue", makeIntConst($3, @1), @1); }
    | MAXVALUE Iconst
        { $$ = makeDefElem("maxvalue", makeIntConst($2, @1), @1); }
    | NO MAXVALUE
        { $$ = makeDefElem("maxvalue", nullptr, @1); }
    | START opt_with Iconst
        { $$ = makeDefElem("start", makeIntConst($3, @1), @1); }
    | START Iconst
        { $$ = makeDefElem("start", makeIntConst($2, @1), @1); }
    | RESTART
        { $$ = makeDefElem("restart", nullptr, @1); }
    | RESTART opt_with Iconst
        { $$ = makeDefElem("restart", makeIntConst($3, @1), @1); }
    | OWNED BY any_name
        { $$ = makeDefElem("owned_by", $3[0], @1); }
    | SEQUENCE NAME_P any_name
        { $$ = makeDefElem("sequence_name", $3[0], @1); }
;

opt_by:
      BY
    | /* empty */
;

opt_with:
      WITH
    | /* empty */
;

AlterSeqStmt:
      ALTER SEQUENCE qualified_name SeqOptList
        {
            AlterSeqStmt* n = makeNode<AlterSeqStmt>();
            n->sequence = static_cast<RangeVar*>($3);
            n->options = std::move($4);
            $$ = n;
        }
    | ALTER SEQUENCE IF_P EXISTS qualified_name SeqOptList
        {
            AlterSeqStmt* n = makeNode<AlterSeqStmt>();
            n->sequence = static_cast<RangeVar*>($5);
            n->options = std::move($6);
            n->missing_ok = true;
            $$ = n;
        }
;

// ===========================================================================
// CreateFunctionStmt / AlterFunctionStmt
// ===========================================================================

CreateFunctionStmt:
      CREATE opt_or_replace FUNCTION function_name func_args_with_defaults
          opt_createfunc_return_type func_as opt_createfunc_opt_list opt_definition
        {
            CreateFunctionStmt* n = makeNode<CreateFunctionStmt>();
            n->is_procedure = false;
            n->replace = $2;
            n->funcname = std::move($4);
            n->parameters = std::move($5);
            n->return_type = static_cast<TypeName*>($6);
            auto fn_options = std::move($8);
            // Convert func_as (AS Sconst list) into a DefElem and prepend.
            if (!$7.empty()) {
                DefElem* as_elem = makeDefElem("as", $7[0], @1);
                fn_options.insert(fn_options.begin(), as_elem);
            }
            n->options = std::move(fn_options);
            (void)$9;
            $$ = n;
        }
    | CREATE opt_or_replace PROCEDURE function_name func_args_with_defaults
          func_as opt_createfunc_opt_list opt_definition
        {
            CreateFunctionStmt* n = makeNode<CreateFunctionStmt>();
            n->is_procedure = true;
            n->replace = $2;
            n->funcname = std::move($4);
            n->parameters = std::move($5);
            auto proc_options = std::move($7);
            if (!$6.empty()) {
                DefElem* as_elem = makeDefElem("as", $6[0], @1);
                proc_options.insert(proc_options.begin(), as_elem);
            }
            n->options = std::move(proc_options);
            (void)$8;
            $$ = n;
        }
;

func_args_with_defaults:
      '(' func_args_with_defaults_list ')'
        { $$ = std::move($2); }
    | '(' ')'
        { $$ = {}; }
;

func_args_with_defaults_list:
      func_arg_with_default
        { $$.push_back($1); }
    | func_args_with_defaults_list ',' func_arg_with_default
        { $1.push_back($3); $$ = std::move($1); }
;

func_arg_with_default:
      func_arg
        { $$ = $1; }
    | func_arg DEFAULT a_expr
        { $$ = $1; }
    | func_arg '=' a_expr
        { $$ = $1; }
;

func_arg:
      func_arg_info
        { $$ = $1; }
    | ColId Typename
        {
            (void)$1;
            $$ = $2;
        }
;

func_arg_info:
      Typename
        { $$ = $1; }
;

opt_createfunc_return_type:
      RETURNS Typename
        { $$ = $2; }
    | RETURNS SETOF Typename
        {
            TypeName* tn = static_cast<TypeName*>($3);
            tn->setof = true;
            $$ = tn;
        }
    | /* empty */
        { $$ = nullptr; }
;

func_as:
      AS func_as_list
        { $$ = std::move($2); }
;

func_as_list:
      Sconst
        { $$.push_back(makeString($1)); }
    | func_as_list ',' Sconst
        { $1.push_back(makeString($3)); $$ = std::move($1); }
;

opt_createfunc_opt_list:
      createfunc_opt_list
        { $$ = std::move($1); }
    | /* empty */
        { $$ = {}; }
;

createfunc_opt_list:
      createfunc_opt_item
        { $$.push_back($1); }
    | createfunc_opt_list createfunc_opt_item
        { $1.push_back($2); $$ = std::move($1); }
;

createfunc_opt_item:
      LANGUAGE nonreservedword_or_sconst
        { $$ = makeDefElem("language", makeString($2), @1); }
    | IMMUTABLE
        { $$ = makeDefElem("volatility", makeString("immutable"), @1); }
    | STABLE
        { $$ = makeDefElem("volatility", makeString("stable"), @1); }
    | VOLATILE
        { $$ = makeDefElem("volatility", makeString("volatile"), @1); }
    | STRICT_P
        { $$ = makeDefElem("strict", makeIntConst(1, @1), @1); }
    | WINDOW
        { $$ = makeDefElem("window", makeIntConst(1, @1), @1); }
    | LEAKPROOF
        { $$ = makeDefElem("leakproof", makeIntConst(1, @1), @1); }
    | SECURITY DEFINER
        { $$ = makeDefElem("security_definer", makeIntConst(1, @1), @1); }
    | SECURITY INVOKER
        { $$ = makeDefElem("security_definer", makeIntConst(0, @1), @1); }
    | PARALLEL nonreservedword_or_sconst
        { $$ = makeDefElem("parallel", makeString($2), @1); }
    | COST NumericOnly
        { $$ = makeDefElem("cost", $2, @1); }
    | ROWS NumericOnly
        { $$ = makeDefElem("rows", $2, @1); }
    | SUPPORT any_name
        { $$ = makeDefElem("support", $2[0], @1); }
;

opt_definition:
      WITH definition
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

definition:
      '(' def_list ')'
        { $$ = std::move($2); }
;

def_list:
      def_elem
        { $$.push_back($1); }
    | def_list ',' def_elem
        { $1.push_back($3); $$ = std::move($1); }
;

nonreservedword_or_sconst:
      nonreservedword
        { $$ = $1; }
    | Sconst
        { $$ = $1; }
;

nonreservedword:
      unreserved_keyword
        { $$ = $1; }
    | col_name_keyword
        { $$ = $1; }
;

NumericOnly:
      FCONST
        { $$ = makeFloatConst($1, @1); }
    | '-' FCONST
        { $$ = makeFloatConst("-" + $2, @1); }
    | SignedIconst
        { $$ = makeIntConst($1, @1); }
;

AlterFunctionStmt:
      ALTER FUNCTION function_name func_args alter_function_opt_list opt_restrict
        {
            AlterFunctionStmt* n = makeNode<AlterFunctionStmt>();
            n->funcname = std::move($3);
            n->args = std::move($4);
            n->actions = std::move($5);
            (void)$6;
            $$ = n;
        }
    | ALTER PROCEDURE function_name func_args alter_function_opt_list opt_restrict
        {
            AlterFunctionStmt* n = makeNode<AlterFunctionStmt>();
            n->funcname = std::move($3);
            n->args = std::move($4);
            n->actions = std::move($5);
            (void)$6;
            $$ = n;
        }
;

func_args:
      '(' func_args_list ')'
        { $$ = std::move($2); }
    | '(' ')'
        { $$ = {}; }
;

func_args_list:
      Typename
        { $$.push_back($1); }
    | func_args_list ',' Typename
        { $1.push_back($3); $$ = std::move($1); }
;

alter_function_opt_list:
      alter_function_opt_item
        { $$.push_back($1); }
    | alter_function_opt_list alter_function_opt_item
        { $1.push_back($2); $$ = std::move($1); }
;

alter_function_opt_item:
      CALLED ON NULL_P INPUT_P
        { $$ = makeDefElem("strict", makeIntConst(0, @1), @1); }
    | RETURNS NULL_P ON NULL_P INPUT_P
        { $$ = makeDefElem("strict", makeIntConst(1, @1), @1); }
    | STRICT_P
        { $$ = makeDefElem("strict", makeIntConst(1, @1), @1); }
    | IMMUTABLE
        { $$ = makeDefElem("volatility", makeString("immutable"), @1); }
    | STABLE
        { $$ = makeDefElem("volatility", makeString("stable"), @1); }
    | VOLATILE
        { $$ = makeDefElem("volatility", makeString("volatile"), @1); }
    | SECURITY DEFINER
        { $$ = makeDefElem("security_definer", makeIntConst(1, @1), @1); }
    | SECURITY INVOKER
        { $$ = makeDefElem("security_definer", makeIntConst(0, @1), @1); }
    | LEAKPROOF
        { $$ = makeDefElem("leakproof", makeIntConst(1, @1), @1); }
    | NOT LEAKPROOF
        { $$ = makeDefElem("leakproof", makeIntConst(0, @1), @1); }
    | COST NumericOnly
        { $$ = makeDefElem("cost", $2, @1); }
    | ROWS NumericOnly
        { $$ = makeDefElem("rows", $2, @1); }
;

opt_restrict:
      RESTRICT
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

// ===========================================================================
// CreateTrigStmt — CREATE TRIGGER
// ===========================================================================

CreateTrigStmt:
      CREATE TRIGGER name trigger_action_time trigger_events ON qualified_name
          trigger_referencing trigger_for_type EXECUTE PROCEDURE function_name '(' trigger_func_args ')'
        {
            CreateTrigStmt* n = makeNode<CreateTrigStmt>();
            n->trigname = $3;
            n->relation = static_cast<RangeVar*>($7);
            n->funcname = std::move($12);
            (void)$4; (void)$5; (void)$8; (void)$9; (void)$14;
            $$ = n;
        }
    | CREATE TRIGGER name trigger_action_time trigger_events ON qualified_name
          trigger_referencing trigger_for_type EXECUTE FUNCTION function_name '(' trigger_func_args ')'
        {
            CreateTrigStmt* n = makeNode<CreateTrigStmt>();
            n->trigname = $3;
            n->relation = static_cast<RangeVar*>($7);
            n->funcname = std::move($12);
            (void)$4; (void)$5; (void)$8; (void)$9; (void)$14;
            $$ = n;
        }
;

trigger_action_time:
      BEFORE
        { $$ = 0; }
    | AFTER
        { $$ = 0; }
    | INSTEAD OF
        { $$ = 0; }
;

trigger_events:
      trigger_event_list
        { $$ = 0; }
;

trigger_event_list:
      trigger_event
    | trigger_event_list OR trigger_event
;

trigger_event:
      INSERT
    | DELETE_P
    | UPDATE
    | UPDATE OF columnList
    | TRUNCATE
;

trigger_referencing:
      REFERENCING trigger_transitions
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

trigger_transitions:
      trigger_transition
    | trigger_transitions trigger_transition
;

trigger_transition:
      transition_old_or_new transition_row_or_table AS ColId
;

transition_old_or_new:
      OLD
    | NEW
;

transition_row_or_table:
      ROW
    | TABLE
;

trigger_for_type:
      FOR EACH ROW
        { $$ = 0; }
    | FOR EACH STATEMENT
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

trigger_func_args:
      trigger_func_arg
        { $$ = 0; }
    | trigger_func_args ',' trigger_func_arg
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

trigger_func_arg:
      Iconst
    | FCONST
    | Sconst
    | ColLabel
;

// ===========================================================================
// CreateRoleStmt / AlterRoleStmt / DropRoleStmt
// ===========================================================================

CreateRoleStmt:
      CREATE ROLE name opt_role_options
        {
            CreateRoleStmt* n = makeNode<CreateRoleStmt>();
            n->stmt_type = CreateRoleStmt::Kind::kRole;
            n->role = $3;
            n->options = std::move($4);
            $$ = n;
        }
    | CREATE ROLE name WITH opt_role_options
        {
            CreateRoleStmt* n = makeNode<CreateRoleStmt>();
            n->stmt_type = CreateRoleStmt::Kind::kRole;
            n->role = $3;
            n->options = std::move($5);
            $$ = n;
        }
    | CREATE USER name opt_role_options
        {
            CreateRoleStmt* n = makeNode<CreateRoleStmt>();
            n->stmt_type = CreateRoleStmt::Kind::kUser;
            n->role = $3;
            n->options = std::move($4);
            $$ = n;
        }
    | CREATE USER name WITH opt_role_options
        {
            CreateRoleStmt* n = makeNode<CreateRoleStmt>();
            n->stmt_type = CreateRoleStmt::Kind::kUser;
            n->role = $3;
            n->options = std::move($5);
            $$ = n;
        }
    | CREATE GROUP_P name opt_role_options
        {
            CreateRoleStmt* n = makeNode<CreateRoleStmt>();
            n->stmt_type = CreateRoleStmt::Kind::kGroup;
            n->role = $3;
            n->options = std::move($4);
            $$ = n;
        }
;

opt_role_options:
      role_options
        { $$ = std::move($1); }
    | /* empty */
        { $$ = {}; }
;

role_options:
      role_option
        { $$.push_back($1); }
    | role_options role_option
        { $1.push_back($2); $$ = std::move($1); }
;

role_option:
      LOGIN
        { $$ = makeDefElem("canlogin", makeIntConst(1, @1), @1); }
    | NO LOGIN
        { $$ = makeDefElem("canlogin", makeIntConst(0, @1), @1); }
    | SUPERUSER_P
        { $$ = makeDefElem("superuser", makeIntConst(1, @1), @1); }
    | NOSUPERUSER
        { $$ = makeDefElem("superuser", makeIntConst(0, @1), @1); }
    | CREATEDB
        { $$ = makeDefElem("createdb", makeIntConst(1, @1), @1); }
    | NOCREATEDB
        { $$ = makeDefElem("createdb", makeIntConst(0, @1), @1); }
    | CREATEROLE
        { $$ = makeDefElem("createrole", makeIntConst(1, @1), @1); }
    | NOCREATEROLE
        { $$ = makeDefElem("createrole", makeIntConst(0, @1), @1); }
    | REPLICATION
        { $$ = makeDefElem("replication", makeIntConst(1, @1), @1); }
    | NOREPLICATION
        { $$ = makeDefElem("replication", makeIntConst(0, @1), @1); }
    | PASSWORD Sconst
        { $$ = makeDefElem("password", makeStrConst($2, @1), @1); }
    | PASSWORD NULL_P
        { $$ = makeDefElem("password", nullptr, @1); }
    | VALID UNTIL Sconst
        { $$ = makeDefElem("valid_until", makeStrConst($3, @1), @1); }
    | CONNECTION LIMIT SignedIconst
        { $$ = makeDefElem("connectionlimit", makeIntConst($3, @1), @1); }
    | IN_P ROLE name_list
        { $$ = makeDefElem("inrole", nullptr, @1); }
    | IN_P GROUP_P name_list
        { $$ = makeDefElem("inrole", nullptr, @1); }
    | ROLE name_list
        { $$ = makeDefElem("rolemembers", nullptr, @1); }
    | ADMIN name_list
        { $$ = makeDefElem("adminmembers", nullptr, @1); }
    | USER name_list
        { $$ = makeDefElem("rolemembers", nullptr, @1); }
    | SYSID Iconst
        { $$ = makeDefElem("sysid", makeIntConst($2, @1), @1); }
;

AlterRoleStmt:
      ALTER ROLE name opt_role_options
        {
            AlterRoleStmt* n = makeNode<AlterRoleStmt>();
            n->options = std::move($4);
            $$ = n;
        }
    | ALTER ROLE name WITH opt_role_options
        {
            AlterRoleStmt* n = makeNode<AlterRoleStmt>();
            n->options = std::move($5);
            $$ = n;
        }
    | ALTER USER name opt_role_options
        {
            AlterRoleStmt* n = makeNode<AlterRoleStmt>();
            n->options = std::move($4);
            $$ = n;
        }
;

DropRoleStmt:
      DROP ROLE name_list
        {
            DropRoleStmt* n = makeNode<DropRoleStmt>();
            n->roles = std::move($3);
            $$ = n;
        }
    | DROP ROLE IF_P EXISTS name_list
        {
            DropRoleStmt* n = makeNode<DropRoleStmt>();
            n->roles = std::move($5);
            n->missing_ok = true;
            $$ = n;
        }
    | DROP USER name_list
        {
            DropRoleStmt* n = makeNode<DropRoleStmt>();
            n->roles = std::move($3);
            $$ = n;
        }
    | DROP GROUP_P name_list
        {
            DropRoleStmt* n = makeNode<DropRoleStmt>();
            n->roles = std::move($3);
            $$ = n;
        }
;

// ===========================================================================
// GrantStmt / RevokeStmt
// ===========================================================================

GrantStmt:
      GRANT privileges ON privilege_target TO grantee_list opt_grant_option
        {
            GrantStmt* n = makeNode<GrantStmt>();
            n->is_grant = true;
            n->privileges = std::move($2);
            (void)$4;
            n->grantees = std::move($6);
            (void)$7;
            $$ = n;
        }
;

RevokeStmt:
      REVOKE privileges ON privilege_target FROM name_list opt_drop_behavior
        {
            GrantStmt* n = makeNode<GrantStmt>();
            n->is_grant = false;
            n->privileges = std::move($2);
            (void)$4;
            $$ = n;
        }
    | REVOKE GRANT OPTION FOR privileges ON privilege_target FROM name_list opt_drop_behavior
        {
            GrantStmt* n = makeNode<GrantStmt>();
            n->is_grant = false;
            n->grant_option = true;
            n->privileges = std::move($5);
            (void)$7;
            $$ = n;
        }
;

privileges:
      privilege_list
        { $$ = std::move($1); }
    | ALL
        { $$ = {}; }
    | ALL PRIVILEGES
        { $$ = {}; }
;

privilege_list:
      privilege
        { $$.push_back($1); }
    | privilege_list ',' privilege
        { $1.push_back($3); $$ = std::move($1); }
;

privilege:
      SELECT
        { $$ = makeNode<AccessPriv>(); }
    | INSERT
        { $$ = makeNode<AccessPriv>(); }
    | UPDATE
        { $$ = makeNode<AccessPriv>(); }
    | DELETE_P
        { $$ = makeNode<AccessPriv>(); }
    | TRUNCATE
        { $$ = makeNode<AccessPriv>(); }
    | REFERENCES
        { $$ = makeNode<AccessPriv>(); }
    | TRIGGER
        { $$ = makeNode<AccessPriv>(); }
    | USAGE
        { $$ = makeNode<AccessPriv>(); }
    | CREATE
        { $$ = makeNode<AccessPriv>(); }
    | CONNECT
        { $$ = makeNode<AccessPriv>(); }
    | TEMPORARY
        { $$ = makeNode<AccessPriv>(); }
    | TEMP
        { $$ = makeNode<AccessPriv>(); }
    | EXECUTE
        { $$ = makeNode<AccessPriv>(); }
;

privilege_target:
      qualified_name_list
        { $$ = std::move($1); }
    | TABLE qualified_name_list
        { $$ = std::move($2); }
    | SEQUENCE qualified_name_list
        { $$ = std::move($2); }
    | SCHEMA qualified_name_list
        { $$ = std::move($2); }
    | DATABASE name_list
        { $$ = {}; }
    | FUNCTION function_name_list
        { $$ = std::move($2); }
;

function_name_list:
      function_name
        { $$ = std::move($1); }
    | function_name_list ',' function_name
        {
            for (Node* n : $3) $1.push_back(n);
            $$ = std::move($1);
        }
;

grantee_list:
      grantee
        { $$.push_back($1); }
    | grantee_list ',' grantee
        { $1.push_back($3); $$ = std::move($1); }
;

grantee:
      RoleSpec
        { $$ = $1; }
    | GROUP_P RoleSpec
        { $$ = $2; }
;

RoleSpec:
      ColId
        {
            RoleSpec* n = makeNode<RoleSpec>();
            n->roletype = RoleSpecType::kCstring;
            n->rolename = $1;
            n->location = @1;
            $$ = n;
        }
    | CURRENT_ROLE
        {
            RoleSpec* n = makeNode<RoleSpec>();
            n->roletype = RoleSpecType::kCurrentRole;
            n->location = @1;
            $$ = n;
        }
    | CURRENT_USER
        {
            RoleSpec* n = makeNode<RoleSpec>();
            n->roletype = RoleSpecType::kCurrentUser;
            n->location = @1;
            $$ = n;
        }
    | SESSION_USER
        {
            RoleSpec* n = makeNode<RoleSpec>();
            n->roletype = RoleSpecType::kSessionUser;
            n->location = @1;
            $$ = n;
        }
;

opt_grant_option:
      WITH GRANT OPTION
        { $$ = true; }
    | /* empty */
        { $$ = false; }
;

// ===========================================================================
// CopyStmt — COPY
// ===========================================================================

CopyStmt:
      COPY opt_binary qualified_name opt_column_list opt_oids copy_from
          opt_program copy_file_name copy_delimiter opt_with copy_options
        {
            CopyStmt* n = makeNode<CopyStmt>();
            n->relation = static_cast<RangeVar*>($3);
            n->attlist = std::move($4);
            n->is_from = $6;
            n->is_program = $7;
            n->filename = $8;
            (void)$2; (void)$5; (void)$9;
            n->options = std::move($11);
            $$ = n;
        }
    | COPY select_with_parens TO copy_file_name opt_with copy_options
        {
            CopyStmt* n = makeNode<CopyStmt>();
            n->query = $2;
            n->is_from = false;
            n->filename = $4;
            n->options = std::move($6);
            $$ = n;
        }
    | COPY select_with_parens FROM copy_file_name opt_with copy_options
        {
            CopyStmt* n = makeNode<CopyStmt>();
            n->query = $2;
            n->is_from = true;
            n->filename = $4;
            n->options = std::move($6);
            $$ = n;
        }
;

opt_binary:
      BINARY
        { $$ = 1; }
    | /* empty */
        { $$ = 0; }
;

opt_oids:
      OIDS
        { $$ = 1; }
    | /* empty */
        { $$ = 0; }
;

copy_from:
      FROM
        { $$ = true; }
    | TO
        { $$ = false; }
;

opt_program:
      PROGRAM
        { $$ = true; }
    | /* empty */
        { $$ = false; }
;

copy_file_name:
      Sconst
        { $$ = $1; }
    | STDIN
        { $$ = ""; }
    | STDOUT
        { $$ = ""; }
;

copy_delimiter:
      opt_using DELIMITER Sconst
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

opt_using:
      USING
    | /* empty */
;

copy_options:
      copy_opt_list
        { $$ = std::move($1); }
    | /* empty */
        { $$ = {}; }
;

copy_opt_list:
      copy_opt_item
        { $$.push_back($1); }
    | copy_opt_list copy_opt_item
        { $1.push_back($2); $$ = std::move($1); }
;

copy_opt_item:
      BINARY
        { $$ = makeDefElem("format", makeString("binary"), @1); }
    | OIDS
        { $$ = makeDefElem("oids", makeIntConst(1, @1), @1); }
    | FREEZE
        { $$ = makeDefElem("freeze", makeIntConst(1, @1), @1); }
    | HEADER_P
        { $$ = makeDefElem("header", makeIntConst(1, @1), @1); }
    | CSV
        { $$ = makeDefElem("format", makeString("csv"), @1); }
    | DELIMITER Sconst
        { $$ = makeDefElem("delimiter", makeStrConst($2, @1), @1); }
    | NULL_P AS Sconst
        { $$ = makeDefElem("null", makeStrConst($3, @1), @1); }
;

// ===========================================================================
// RefreshMatViewStmt — REFRESH MATERIALIZED VIEW
// ===========================================================================

RefreshMatViewStmt:
      REFRESH MATERIALIZED VIEW opt_concurrently qualified_name opt_with_data
        {
            RefreshMatViewStmt* n = makeNode<RefreshMatViewStmt>();
            n->concurrent = $4;
            n->relation = static_cast<RangeVar*>($5);
            (void)$6;
            $$ = n;
        }
    | REFRESH MATERIALIZED VIEW IF_P NOT EXISTS qualified_name opt_with_data
        {
            RefreshMatViewStmt* n = makeNode<RefreshMatViewStmt>();
            n->relation = static_cast<RangeVar*>($7);
            (void)$8;
            $$ = n;
        }
;

// ===========================================================================
// CreateTableSpaceStmt / DropTableSpaceStmt
// ===========================================================================

CreateTableSpaceStmt:
      CREATE TABLESPACE name opt_tablespace_owner LOCATION Sconst opt_reloptions
        {
            CreateTableSpaceStmt* n = makeNode<CreateTableSpaceStmt>();
            n->tablespacename = $3;
            (void)$4;
            n->location = $6;
            n->options = std::move($7);
            $$ = n;
        }
;

opt_tablespace_owner:
      OWNER name
        { $$ = 0; }
    | OWNER CURRENT_USER
        { $$ = 0; }
    | OWNER SESSION_USER
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

DropTableSpaceStmt:
      DROP TABLESPACE name
        {
            DropTableSpaceStmt* n = makeNode<DropTableSpaceStmt>();
            n->tablespacename = $3;
            $$ = n;
        }
    | DROP TABLESPACE IF_P EXISTS name
        {
            DropTableSpaceStmt* n = makeNode<DropTableSpaceStmt>();
            n->tablespacename = $5;
            n->missing_ok = true;
            $$ = n;
        }
;

// ===========================================================================
// CreatedbStmt / DropdbStmt / AlterDatabaseStmt
// ===========================================================================

CreatedbStmt:
      CREATE DATABASE name opt_createdb_opt_list
        {
            CreatedbStmt* n = makeNode<CreatedbStmt>();
            n->dbname = $3;
            n->options = std::move($4);
            $$ = n;
        }
;

opt_createdb_opt_list:
      createdb_opt_list
        { $$ = std::move($1); }
    | /* empty */
        { $$ = {}; }
;

createdb_opt_list:
      createdb_opt_item
        { $$.push_back($1); }
    | createdb_opt_list createdb_opt_item
        { $1.push_back($2); $$ = std::move($1); }
;

createdb_opt_item:
      OWNER name
        { $$ = makeDefElem("owner", makeString($2), @1); }
    | OWNER CURRENT_USER
        { $$ = makeDefElem("owner", makeString("current_user"), @1); }
    | OWNER SESSION_USER
        { $$ = makeDefElem("owner", makeString("session_user"), @1); }
    | TEMPLATE name
        { $$ = makeDefElem("template", makeString($2), @1); }
    | ENCODING Sconst
        { $$ = makeDefElem("encoding", makeStrConst($2, @1), @1); }
    | LC_COLLATE_P Sconst
        { $$ = makeDefElem("lc_collate", makeStrConst($2, @1), @1); }
    | LC_CTYPE_P Sconst
        { $$ = makeDefElem("lc_ctype", makeStrConst($2, @1), @1); }
    | TABLESPACE name
        { $$ = makeDefElem("tablespace", makeString($2), @1); }
    | CONNECTION LIMIT SignedIconst
        { $$ = makeDefElem("connectionlimit", makeIntConst($3, @1), @1); }
;

DropdbStmt:
      DROP DATABASE name
        {
            DropdbStmt* n = makeNode<DropdbStmt>();
            n->dbname = $3;
            $$ = n;
        }
    | DROP DATABASE IF_P EXISTS name
        {
            DropdbStmt* n = makeNode<DropdbStmt>();
            n->dbname = $5;
            n->missing_ok = true;
            $$ = n;
        }
    | DROP DATABASE name WITH '(' createdb_opt_list ')'
        {
            DropdbStmt* n = makeNode<DropdbStmt>();
            n->dbname = $3;
            n->options = std::move($6);
            $$ = n;
        }
;

AlterDatabaseStmt:
      ALTER DATABASE name opt_createdb_opt_list
        {
            AlterDatabaseStmt* n = makeNode<AlterDatabaseStmt>();
            n->dbname = $3;
            n->options = std::move($4);
            $$ = n;
        }
    | ALTER DATABASE name SET TABLESPACE name
        {
            AlterDatabaseStmt* n = makeNode<AlterDatabaseStmt>();
            n->dbname = $3;
            n->options.push_back(makeDefElem("tablespace", makeString($6), @1));
            $$ = n;
        }
;

any_name_list:
      any_name
        { $$.push_back($1[0]); }
    | any_name_list ',' any_name
        {
            for (Node* n : $3) $1.push_back(n);
            $$ = std::move($1);
        }
;

any_name:
      ColId attrs
        {
            std::vector<Node*> v;
            v.push_back(makeString($1));
            for (Node* n : $2) v.push_back(n);
            $$ = std::move(v);
        }
    | ColId
        {
            std::vector<Node*> v;
            v.push_back(makeString($1));
            $$ = std::move(v);
        }
;

attrs:
      '.' attr_name
        { $$.push_back(makeString($2)); }
    | attrs '.' attr_name
        { $1.push_back(makeString($3)); $$ = std::move($1); }
;

attr_name:
      ColLabel
        { $$ = $1; }
;

// Target list.
target_list:
      target_el
        { $$.push_back($1); }
    | target_list ',' target_el
        { $1.push_back($3); $$ = std::move($1); }
;

target_el:
      a_expr AS ColLabel
        {
            ResTarget* n = makeNode<ResTarget>();
            n->name = $3;
            n->val = $1;
            n->location = @1;
            $$ = n;
        }
    | a_expr IDENT
        {
            ResTarget* n = makeNode<ResTarget>();
            n->name = $2;
            n->val = $1;
            n->location = @1;
            $$ = n;
        }
    | a_expr
        {
            ResTarget* n = makeNode<ResTarget>();
            n->val = $1;
            n->location = @1;
            $$ = n;
        }
    | '*'
        {
            ResTarget* n = makeNode<ResTarget>();
            n->val = makeNode<AStar>();
            n->location = @1;
            $$ = n;
        }
;

// Expressions.
a_expr:
      c_expr
    | a_expr TYPECAST Typename
        { $$ = makeTypeCast($1, $3, @2); }
    | a_expr COLLATE any_name
        {
            CollateClause* n = makeNode<CollateClause>();
            n->arg = $1;
            n->collname = std::move($3);
            n->location = @2;
            $$ = n;
        }
    | '+' a_expr %prec UMINUS
        { $$ = $2; }
    | '-' a_expr %prec UMINUS
        { $$ = doNegate($2, @1); }
    | a_expr '+' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "+", $1, $3, @2); }
    | a_expr '-' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "-", $1, $3, @2); }
    | a_expr '*' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "*", $1, $3, @2); }
    | a_expr '/' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "/", $1, $3, @2); }
    | a_expr '%' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "%", $1, $3, @2); }
    | a_expr '^' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "^", $1, $3, @2); }
    | a_expr '<' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "<", $1, $3, @2); }
    | a_expr '>' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, ">", $1, $3, @2); }
    | a_expr '=' a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "=", $1, $3, @2); }
    | a_expr LESS_EQUALS a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "<=", $1, $3, @2); }
    | a_expr GREATER_EQUALS a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, ">=", $1, $3, @2); }
    | a_expr NOT_EQUALS a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "<>", $1, $3, @2); }
    | a_expr AND a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "AND", $1, $3, @2); }
    | a_expr OR a_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "OR", $1, $3, @2); }
    | NOT a_expr
        { $$ = makeNotExpr($2); }
    | a_expr LIKE a_expr
        { $$ = makeSimpleAExpr(AExprKind::kLike, "~~", $1, $3, @2); }
    | a_expr LIKE a_expr ESCAPE a_expr
        { $$ = makeSimpleAExpr(AExprKind::kLike, "~~", $1, $3, @2); }
    | a_expr ILIKE a_expr
        { $$ = makeSimpleAExpr(AExprKind::kIlike, "~~*", $1, $3, @2); }
    | a_expr ILIKE a_expr ESCAPE a_expr
        { $$ = makeSimpleAExpr(AExprKind::kIlike, "~~*", $1, $3, @2); }
    | a_expr SIMILAR TO a_expr ESCAPE a_expr
        { $$ = makeSimpleAExpr(AExprKind::kSimilar, "~", $1, $4, @2); }
    | a_expr SIMILAR TO a_expr
        { $$ = makeSimpleAExpr(AExprKind::kSimilar, "~", $1, $4, @2); }
    | a_expr NOT LIKE a_expr
        { $$ = makeSimpleAExpr(AExprKind::kLike, "!~~", $1, $4, @2); }
    | a_expr NOT ILIKE a_expr
        { $$ = makeSimpleAExpr(AExprKind::kIlike, "!~~*", $1, $4, @2); }
    | a_expr NOT SIMILAR TO a_expr
        { $$ = makeSimpleAExpr(AExprKind::kSimilar, "!~", $1, $5, @2); }
    | a_expr IS NULL_P
        {
            NullTest* n = makeNode<NullTest>();
            n->arg = $1;
            n->nulltesttype = NullTestType::kIsNull;
            n->location = @2;
            $$ = n;
        }
    | a_expr IS NOT NULL_P
        {
            NullTest* n = makeNode<NullTest>();
            n->arg = $1;
            n->nulltesttype = NullTestType::kIsNotNull;
            n->location = @2;
            $$ = n;
        }
    | a_expr ISNULL
        {
            NullTest* n = makeNode<NullTest>();
            n->arg = $1;
            n->nulltesttype = NullTestType::kIsNull;
            n->location = @2;
            $$ = n;
        }
    | a_expr NOTNULL
        {
            NullTest* n = makeNode<NullTest>();
            n->arg = $1;
            n->nulltesttype = NullTestType::kIsNotNull;
            n->location = @2;
            $$ = n;
        }
    | a_expr IS TRUE_P
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS TRUE", $1, nullptr, @2); }
    | a_expr IS NOT TRUE_P
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS NOT TRUE", $1, nullptr, @2); }
    | a_expr IS FALSE_P
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS FALSE", $1, nullptr, @2); }
    | a_expr IS NOT FALSE_P
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS NOT FALSE", $1, nullptr, @2); }
    | a_expr IS UNKNOWN
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS UNKNOWN", $1, nullptr, @2); }
    | a_expr IS NOT UNKNOWN
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS NOT UNKNOWN", $1, nullptr, @2); }
    | a_expr IS DISTINCT FROM a_expr
        { $$ = makeSimpleAExpr(AExprKind::kDistinct, "<>", $1, $5, @2); }
    | a_expr IS NOT DISTINCT FROM a_expr
        { $$ = makeSimpleAExpr(AExprKind::kNotDistinct, "<>", $1, $6, @2); }
    | a_expr IN_P in_expr
        { $$ = makeSimpleAExpr(AExprKind::kIn, "=", $1, $3, @2); }
    | a_expr NOT IN_P in_expr
        { $$ = makeSimpleAExpr(AExprKind::kIn, "<>", $1, $4, @2); }
    | a_expr BETWEEN opt_asymmetric b_expr AND b_expr
        { $$ = makeSimpleAExpr(AExprKind::kBetween, "BETWEEN", $1, $4, @2); (void)$6; }
    | a_expr NOT BETWEEN opt_asymmetric b_expr AND b_expr
        { $$ = makeSimpleAExpr(AExprKind::kNotBetween, "NOT BETWEEN", $1, $5, @2); (void)$7; }
    | a_expr BETWEEN SYMMETRIC b_expr AND b_expr
        { $$ = makeSimpleAExpr(AExprKind::kBetweenSym, "BETWEEN SYMMETRIC", $1, $4, @2); (void)$6; }
    | a_expr NOT BETWEEN SYMMETRIC b_expr AND b_expr
        { $$ = makeSimpleAExpr(AExprKind::kNotBetweenSym, "NOT BETWEEN SYMMETRIC", $1, $5, @2); (void)$7; }
    | a_expr qual_Op a_expr
        { $$ = makeAExpr(AExprKind::kOp, std::move($2), $1, $3, @2); }
    | qual_Op a_expr %prec UMINUS
        { $$ = makeAExpr(AExprKind::kOp, std::move($1), nullptr, $2, @1); }
;

opt_asymmetric:
      ASYMMETRIC
    | SYMMETRIC
    | /* empty */
;

b_expr:
      c_expr
    | b_expr TYPECAST Typename
        { $$ = makeTypeCast($1, $3, @2); }
    | '+' b_expr %prec UMINUS
        { $$ = $2; }
    | '-' b_expr %prec UMINUS
        { $$ = doNegate($2, @1); }
    | b_expr '+' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "+", $1, $3, @2); }
    | b_expr '-' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "-", $1, $3, @2); }
    | b_expr '*' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "*", $1, $3, @2); }
    | b_expr '/' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "/", $1, $3, @2); }
    | b_expr '%' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "%", $1, $3, @2); }
    | b_expr '^' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "^", $1, $3, @2); }
    | b_expr '<' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "<", $1, $3, @2); }
    | b_expr '>' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, ">", $1, $3, @2); }
    | b_expr '=' b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "=", $1, $3, @2); }
    | b_expr LESS_EQUALS b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "<=", $1, $3, @2); }
    | b_expr GREATER_EQUALS b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, ">=", $1, $3, @2); }
    | b_expr NOT_EQUALS b_expr
        { $$ = makeSimpleAExpr(AExprKind::kOp, "<>", $1, $3, @2); }
    | b_expr qual_Op b_expr
        { $$ = makeAExpr(AExprKind::kOp, std::move($2), $1, $3, @2); }
    | qual_Op b_expr %prec UMINUS
        { $$ = makeAExpr(AExprKind::kOp, std::move($1), nullptr, $2, @1); }
    | b_expr IS DISTINCT FROM b_expr
        { $$ = makeSimpleAExpr(AExprKind::kDistinct, "<>", $1, $5, @2); }
    | b_expr IS NOT DISTINCT FROM b_expr
        { $$ = makeSimpleAExpr(AExprKind::kNotDistinct, "<>", $1, $6, @2); }
    | b_expr IS NULL_P
        {
            NullTest* n = makeNode<NullTest>();
            n->arg = $1;
            n->nulltesttype = NullTestType::kIsNull;
            n->location = @2;
            $$ = n;
        }
    | b_expr IS NOT NULL_P
        {
            NullTest* n = makeNode<NullTest>();
            n->arg = $1;
            n->nulltesttype = NullTestType::kIsNotNull;
            n->location = @2;
            $$ = n;
        }
    | b_expr IS TRUE_P
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS TRUE", $1, nullptr, @2); }
    | b_expr IS NOT TRUE_P
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS NOT TRUE", $1, nullptr, @2); }
    | b_expr IS FALSE_P
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS FALSE", $1, nullptr, @2); }
    | b_expr IS NOT FALSE_P
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS NOT FALSE", $1, nullptr, @2); }
    | b_expr IS UNKNOWN
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS UNKNOWN", $1, nullptr, @2); }
    | b_expr IS NOT UNKNOWN
        { $$ = makeSimpleAExpr(AExprKind::kOp, "IS NOT UNKNOWN", $1, nullptr, @2); }
;

c_expr:
      columnref
    | AexprConst
    | PARAM opt_indirection
        {
            ParamRef* n = makeNode<ParamRef>();
            n->number = $1;
            n->location = @1;
            if (!$2.empty()) {
                AIndirection* ind = makeNode<AIndirection>();
                ind->arg = n;
                ind->indirection = std::move($2);
                $$ = ind;
            } else {
                $$ = n;
            }
        }
    | '(' a_expr ')'
        { $$ = $2; }
    | case_expr
    | func_expr
    | select_with_parens
        {
            SubLink* n = makeNode<SubLink>();
            n->sublinktype = SubLinkType::kExpr;
            n->subselect = $1;
            n->location = @1;
            $$ = n;
        }
    | select_with_parens indirection
        {
            AIndirection* n = makeNode<AIndirection>();
            n->arg = $1;
            n->indirection = std::move($2);
            $$ = n;
        }
    | EXISTS select_with_parens
        {
            SubLink* n = makeNode<SubLink>();
            n->sublinktype = SubLinkType::kExists;
            n->subselect = $2;
            n->location = @1;
            $$ = n;
        }
    | ARRAY select_with_parens
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("array"));
            $$ = makeFuncCall(std::move(fn), {$2}, @1);
        }
    | ARRAY array_expr
        { $$ = $2; }
;

columnref:
      ColId
        {
            std::vector<Node*> f;
            f.push_back(makeString($1));
            $$ = makeColumnRef(std::move(f), @1);
        }
    | ColId indirection
        {
            std::vector<Node*> f;
            f.push_back(makeString($1));
            for (Node* n : $2) f.push_back(n);
            $$ = makeColumnRef(std::move(f), @1);
        }
;

indirection_el:
      '.' attr_name
        { $$ = makeString($2); }
    | '.' '*'
        { $$ = makeNode<AStar>(); }
    | '[' a_expr ']'
        {
            AIndices* n = makeNode<AIndices>();
            n->uidx = $2;
            $$ = n;
        }
    | '[' opt_slice_bound ':' opt_slice_bound ']'
        {
            AIndices* n = makeNode<AIndices>();
            n->is_slice = true;
            n->lidx = $2;
            n->uidx = $4;
            $$ = n;
        }
;

opt_slice_bound:
      a_expr
    | /* empty */
        { $$ = nullptr; }
;

indirection:
      indirection_el
        { $$.push_back($1); }
    | indirection indirection_el
        { $1.push_back($2); $$ = std::move($1); }
;

// CASE expression.
case_expr:
      CASE case_arg when_clause_list case_default END_P
        {
            CaseExpr* n = makeNode<CaseExpr>();
            n->arg = $2;
            n->args = std::move($3);
            n->defresult = $4;
            n->location = @1;
            $$ = n;
        }
;

case_arg:
      a_expr
    | /* empty */
        { $$ = nullptr; }
;

when_clause_list:
      when_clause
        { $$.push_back($1); }
    | when_clause_list when_clause
        { $1.push_back($2); $$ = std::move($1); }
;

when_clause:
      WHEN a_expr THEN a_expr
        {
            CaseWhen* w = makeNode<CaseWhen>();
            w->expr = $2;
            w->result = $4;
            w->location = @1;
            $$ = w;
        }
;

case_default:
      ELSE a_expr
        { $$ = $2; }
    | /* empty */
        { $$ = nullptr; }
;

// Function calls.
func_application:
      func_name '(' ')'
        { $$ = makeFuncCall(std::move($1), {}, @1); }
    | func_name '(' func_arg_list opt_sort_clause ')'
        {
            FuncCall* n = static_cast<FuncCall*>(
                makeFuncCall(std::move($1), std::move($3), @1));
            n->agg_order = std::move($4);
            $$ = n;
        }
    | func_name '(' VARIADIC func_arg_expr ')'
        {
            FuncCall* n = static_cast<FuncCall*>(
                makeFuncCall(std::move($1), {$4}, @1));
            n->func_variadic = true;
            $$ = n;
        }
    | func_name '(' '*' ')'
        {
            FuncCall* n = static_cast<FuncCall*>(
                makeFuncCall(std::move($1), {}, @1));
            n->agg_star = true;
            $$ = n;
        }
    | func_name '(' DISTINCT func_arg_list ')'
        {
            FuncCall* n = static_cast<FuncCall*>(
                makeFuncCall(std::move($1), std::move($4), @1));
            n->agg_distinct = true;
            $$ = n;
        }
;

func_arg_list:
      func_arg_expr
        { $$.push_back($1); }
    | func_arg_list ',' func_arg_expr
        { $1.push_back($3); $$ = std::move($1); }
;

func_arg_expr:
      a_expr
    | param_name COLON_EQUALS a_expr
        { $$ = $3; }
    | param_name EQUALS_GREATER a_expr
        { $$ = $3; }
;

param_name:
      ColId
        { $$ = $1; }
    | type_function_name
        { $$ = $1; }
;

func_expr:
      func_application opt_within_group_clause opt_filter_clause opt_over_clause
        {
            FuncCall* n = static_cast<FuncCall*>($1);
            if ($2 != nullptr) n->agg_within_group = true;
            n->agg_filter = $3;
            n->over = static_cast<WindowDef*>($4);
            $$ = $1;
        }
    | func_expr_common_subexpr
;

func_expr_common_subexpr:
      COLLATION FOR '(' a_expr ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_collation_for"));
            $$ = makeFuncCall(std::move(fn), {$4}, @1);
        }
    | CURRENT_DATE
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("current_date"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | CURRENT_TIME
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("current_time"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | CURRENT_TIMESTAMP
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("current_timestamp"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | LOCALTIME
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("localtime"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | LOCALTIMESTAMP
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("localtimestamp"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | CURRENT_ROLE
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("current_role"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | CURRENT_USER
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("current_user"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | SESSION_USER
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("session_user"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | CURRENT_CATALOG
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("current_catalog"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | CURRENT_SCHEMA
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("current_schema"));
            $$ = makeFuncCall(std::move(fn), {}, @1);
        }
    | CAST '(' a_expr AS Typename ')'
        { $$ = makeTypeCast($3, $5, @1); }
    | EXTRACT '(' extract_arg FROM a_expr ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("extract"));
            std::vector<Node*> args;
            args.push_back(makeStrConst($3, @3));
            args.push_back($5);
            $$ = makeFuncCall(std::move(fn), std::move(args), @1);
        }
    | OVERLAY '(' overlay_list ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("overlay"));
            $$ = makeFuncCall(std::move(fn), {$3}, @1);
        }
    | POSITION '(' position_list ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("position"));
            $$ = makeFuncCall(std::move(fn), {$3}, @1);
        }
    | SUBSTRING '(' substr_list ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("substring"));
            $$ = makeFuncCall(std::move(fn), {$3}, @1);
        }
    | TREAT '(' a_expr AS Typename ')'
        { $$ = makeTypeCast($3, $5, @1); }
    | TRIM '(' trim_list ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("btrim"));
            $$ = makeFuncCall(std::move(fn), std::move($3), @1);
        }
    | NULLIF '(' a_expr ',' a_expr ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("nullif"));
            $$ = makeFuncCall(std::move(fn), {$3, $5}, @1);
        }
    | COALESCE '(' expr_list ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("coalesce"));
            $$ = makeFuncCall(std::move(fn), std::move($3), @1);
        }
    | GREATEST '(' expr_list ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("greatest"));
            $$ = makeFuncCall(std::move(fn), std::move($3), @1);
        }
    | LEAST '(' expr_list ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("pg_catalog"));
            fn.push_back(makeString("least"));
            $$ = makeFuncCall(std::move(fn), std::move($3), @1);
        }
    | XMLCONCAT '(' expr_list ')'
        {
            std::vector<Node*> fn;
            fn.push_back(makeString("xmlconcat"));
            $$ = makeFuncCall(std::move(fn), std::move($3), @1);
        }
;

within_group_clause:
      WITHIN GROUP_P '(' sort_clause ')'
        { $$ = nullptr; }
;

opt_within_group_clause:
      within_group_clause
    | /* empty */
        { $$ = nullptr; }
;

filter_clause:
      FILTER '(' WHERE a_expr ')'
        { $$ = $4; }
;

opt_filter_clause:
      filter_clause
    | /* empty */
        { $$ = nullptr; }
;

extract_arg:
      IDENT
        { $$ = $1; }
    | YEAR_P
        { $$ = "year"; }
    | MONTH_P
        { $$ = "month"; }
    | DAY_P
        { $$ = "day"; }
    | HOUR_P
        { $$ = "hour"; }
    | MINUTE_P
        { $$ = "minute"; }
    | SECOND_P
        { $$ = "second"; }
;

overlay_list:
      a_expr overlay_placing substr_from substr_for
        { $$ = $1; }
    | a_expr overlay_placing substr_for
        { $$ = $1; }
;

overlay_placing:
      PLACING a_expr
;

substr_from:
      FROM a_expr
;

substr_for:
      FOR a_expr
;

position_list:
      b_expr IN_P b_expr
        { $$ = $1; }
    | /* empty */
        { $$ = nullptr; }
;

substr_list:
      a_expr substr_from substr_for
        { $$ = $1; }
    | a_expr substr_for substr_from
        { $$ = $1; }
    | a_expr substr_from
        { $$ = $1; }
    | a_expr FROM a_expr FOR a_expr
        { $$ = $1; }
    | a_expr FOR a_expr FROM a_expr
        { $$ = $1; }
    | a_expr FOR a_expr
        { $$ = $1; }
    | a_expr FROM a_expr
        { $$ = $1; }
    | /* empty */
        { $$ = nullptr; }
;

trim_list:
      a_expr FROM expr_list
        { $$ = std::move($3); $$.insert($$.begin(), $1); }
    | FROM expr_list
        { $$ = std::move($2); }
    | expr_list
        { $$ = std::move($1); }
;

in_expr:
      select_with_parens
        { $$ = $1; }
    | '(' expr_list ')'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->elements = std::move($2);
            n->location = @1;
            $$ = n;
        }
;

sub_type:
      select_with_parens
        { $$ = $1; }
    | '(' expr_list ')'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->elements = std::move($2);
            n->location = @1;
            $$ = n;
        }
;

subquery_Op:
      all_Op
        { $$ = makeNode<AExpr>(); (void)$1; }
    | OPERATOR '(' any_operator ')'
        { $$ = makeNode<AExpr>(); }
;

all_Op:
      Op
        { $$ = $1; }
    | '+'
        { $$ = "+"; }
    | '-'
        { $$ = "-"; }
    | '*'
        { $$ = "*"; }
    | '/'
        { $$ = "/"; }
    | '%'
        { $$ = "%"; }
    | '^'
        { $$ = "^"; }
    | '<'
        { $$ = "<"; }
    | '>'
        { $$ = ">"; }
    | '='
        { $$ = "="; }
    | LESS_EQUALS
        { $$ = "<="; }
    | GREATER_EQUALS
        { $$ = ">="; }
    | NOT_EQUALS
        { $$ = "<>"; }
;

qual_Op:
      Operator
        { $$.push_back(makeString($1)); }
;

Operator:
      Op
        { $$ = $1; }
;

// Constants.
AexprConst:
      Iconst
        { $$ = makeIntConst($1, @1); }
    | FCONST
        { $$ = makeFloatConst($1, @1); }
    | SCONST
        { $$ = makeStrConst($1, @1); }
    | BCONST
        { $$ = makeBitStringConst($1, @1); }
    | XCONST
        { $$ = makeBitStringConst($1, @1); }
    | func_name SCONST
        { $$ = makeStrConst($2, @2); }
    | TRUE_P
        { $$ = makeBoolAConst(true, @1); }
    | FALSE_P
        { $$ = makeBoolAConst(false, @1); }
    | NULL_P
        { $$ = makeNullAConst(@1); }
;

Iconst:
      ICONST
        { $$ = $1; }
;

Sconst:
      SCONST
        { $$ = $1; }
;

SignedIconst:
      Iconst
        { $$ = $1; }
    | '+' Iconst
        { $$ = $2; }
    | '-' Iconst
        { $$ = -$2; }
;

// ROW and ARRAY expressions.
row:
      ROW '(' expr_list ')'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->elements = std::move($3);
            n->location = @1;
            $$ = n;
        }
    | ROW '(' ')'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->location = @1;
            $$ = n;
        }
    | '(' expr_list ',' a_expr ')'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->elements = std::move($2);
            n->elements.push_back($4);
            n->location = @1;
            $$ = n;
        }
;

array_expr:
      '[' expr_list ']'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->elements = std::move($2);
            n->location = @1;
            $$ = n;
        }
    | '[' ']'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->location = @1;
            $$ = n;
        }
    | ARRAY '[' expr_list ']'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->elements = std::move($3);
            n->location = @1;
            $$ = n;
        }
    | ARRAY '[' ']'
        {
            AArrayExpr* n = makeNode<AArrayExpr>();
            n->location = @1;
            $$ = n;
        }
;

// Expression list.
expr_list:
      a_expr
        { $$.push_back($1); }
    | expr_list ',' a_expr
        { $1.push_back($3); $$ = std::move($1); }
;

// Type list (for IS OF).
type_list:
      Typename
        { $$.push_back($1); }
    | type_list ',' Typename
        { $1.push_back($3); $$ = std::move($1); }
;

// Type names.
Typename:
      SimpleTypename opt_array_bounds
        {
            (void)$2;
            $$ = $1;
        }
    | SETOF SimpleTypename opt_array_bounds
        {
            static_cast<TypeName*>($2)->setof = true;
            (void)$3;
            $$ = $2;
        }
    | SimpleTypename ARRAY '[' Iconst ']'
        {
            TypeName* n = static_cast<TypeName*>($1);
            n->array_bounds.push_back(makeInteger(static_cast<int64_t>($4)));
            $$ = n;
        }
    | SimpleTypename ARRAY
        {
            static_cast<TypeName*>($1)->array_bounds.push_back(makeInteger(-1));
            $$ = $1;
        }
;

opt_array_bounds:
      opt_array_bounds '[' ']'
        { $$ = 0; }
    | opt_array_bounds '[' Iconst ']'
        { $$ = static_cast<int>($3); }
    | /* empty */
        { $$ = 0; }
;

SimpleTypename:
      GenericType
    | Numeric
    | Bit
    | Character
    | ConstDatetime
    | ConstInterval
    | ConstTypename
;

ConstTypename:
      Numeric
    | ConstBit
    | ConstCharacter
    | ConstDatetime
;

GenericType:
      type_function_name opt_type_modifiers opt_array_bounds
        {
            TypeName* n = makeTypeName({}, @1);
            n->names.push_back(makeString($1));
            n->typmods = std::move($2);
            (void)$3;
            $$ = n;
        }
    | type_function_name attrs opt_type_modifiers opt_array_bounds
        {
            TypeName* n = makeTypeName({}, @1);
            n->names.push_back(makeString($1));
            for (Node* a : $2) n->names.push_back(a);
            n->typmods = std::move($3);
            (void)$4;
            $$ = n;
        }
;

opt_type_modifiers:
      '(' expr_list ')'
        { $$ = std::move($2); }
    | /* empty */
        { $$ = {}; }
;

Numeric:
      INT_P
        { $$ = SystemTypeName("int4"); }
    | INTEGER
        { $$ = SystemTypeName("int4"); }
    | SMALLINT
        { $$ = SystemTypeName("int2"); }
    | BIGINT
        { $$ = SystemTypeName("int8"); }
    | REAL
        { $$ = SystemTypeName("float4"); }
    | FLOAT_P opt_float
        { $$ = SystemTypeName("float4"); (void)$2; }
    | DOUBLE_P PRECISION
        { $$ = SystemTypeName("float8"); }
    | DECIMAL_P opt_type_modifiers
        {
            TypeName* n = SystemTypeName("numeric");
            n->typmods = std::move($2);
            $$ = n;
        }
    | DEC opt_type_modifiers
        {
            TypeName* n = SystemTypeName("numeric");
            n->typmods = std::move($2);
            $$ = n;
        }
    | NUMERIC opt_type_modifiers
        {
            TypeName* n = SystemTypeName("numeric");
            n->typmods = std::move($2);
            $$ = n;
        }
    | BOOLEAN_P
        { $$ = SystemTypeName("bool"); }
;

opt_float:
      '(' Iconst ')'
        { $$ = $2; }
    | /* empty */
        { $$ = 0; }
;

Bit:
      BIT opt_varying opt_type_modifiers
        {
            TypeName* n = SystemTypeName($2 ? "varbit" : "bit");
            n->typmods = std::move($3);
            $$ = n;
        }
;

ConstBit:
      BIT opt_type_modifiers
        {
            TypeName* n = SystemTypeName("bit");
            n->typmods = std::move($2);
            $$ = n;
        }
;

Character:
      character opt_c_string
        { $$ = $1; }
;

ConstCharacter:
      character opt_c_string
        { $$ = $1; }
;

character:
      CHARACTER opt_varying opt_type_modifiers
        {
            TypeName* n = SystemTypeName($2 ? "varchar" : "bpchar");
            n->typmods = std::move($3);
            $$ = n;
        }
    | CHAR_P opt_varying opt_type_modifiers
        {
            TypeName* n = SystemTypeName($2 ? "varchar" : "bpchar");
            n->typmods = std::move($3);
            $$ = n;
        }
    | VARCHAR opt_type_modifiers
        {
            TypeName* n = SystemTypeName("varchar");
            n->typmods = std::move($2);
            $$ = n;
        }
    | NATIONAL CHAR_P opt_varying opt_type_modifiers
        {
            TypeName* n = SystemTypeName($3 ? "varchar" : "bpchar");
            n->typmods = std::move($4);
            $$ = n;
        }
    | NATIONAL CHARACTER opt_varying opt_type_modifiers
        {
            TypeName* n = SystemTypeName($3 ? "varchar" : "bpchar");
            n->typmods = std::move($4);
            $$ = n;
        }
    | NCHAR opt_varying opt_type_modifiers
        {
            TypeName* n = SystemTypeName($2 ? "varchar" : "bpchar");
            n->typmods = std::move($3);
            $$ = n;
        }
;

opt_c_string:
      opt_type_modifiers
        { $$ = $1; }
    | /* empty */
        { $$ = {}; }
;

opt_varying:
      VARYING
        { $$ = 1; }
    | /* empty */
        { $$ = 0; }
;

ConstDatetime:
      TIMESTAMP '(' Iconst ')' opt_timezone
        {
            TypeName* n = SystemTypeName("timestamp");
            n->typmods.push_back(makeInteger(static_cast<int64_t>($3)));
            (void)$5;
            $$ = n;
        }
    | TIMESTAMP opt_timezone
        { $$ = SystemTypeName("timestamp"); (void)$2; }
    | TIME '(' Iconst ')' opt_timezone
        {
            TypeName* n = SystemTypeName("time");
            n->typmods.push_back(makeInteger(static_cast<int64_t>($3)));
            (void)$5;
            $$ = n;
        }
    | TIME opt_timezone
        { $$ = SystemTypeName("time"); (void)$2; }
;

opt_timezone:
      WITH TIME ZONE
        { $$ = 1; }
    | WITHOUT TIME ZONE
        { $$ = 0; }
    | /* empty */
        { $$ = 0; }
;

ConstInterval:
      INTERVAL
        { $$ = SystemTypeName("interval"); }
    | INTERVAL interval_qualifier
        { $$ = SystemTypeName("interval"); }
;

interval_qualifier:
      YEAR_P
    | MONTH_P
    | DAY_P
    | HOUR_P
    | MINUTE_P
    | SECOND_P
    | YEAR_P TO MONTH_P
    | DAY_P TO HOUR_P
    | DAY_P TO MINUTE_P
    | DAY_P TO SECOND_P
    | HOUR_P TO MINUTE_P
    | HOUR_P TO SECOND_P
    | MINUTE_P TO SECOND_P
;

// Function name.
func_name:
      type_function_name
        { $$.push_back(makeString($1)); }
    | ColId indirection
        {
            $$.push_back(makeString($1));
            for (Node* n : $2) $$.push_back(n);
        }
;

// Name lists.
name_list:
      name
        { $$.push_back(makeString($1)); }
    | name_list ',' name
        { $1.push_back(makeString($3)); $$ = std::move($1); }
;

name:
      ColId
        { $$ = $1; }
    | unreserved_keyword
        { $$ = $1; }
    | col_name_keyword
        { $$ = $1; }
;

ColId:
      IDENT
        { $$ = $1; }
    | unreserved_keyword
        { $$ = $1; }
    | col_name_keyword
        { $$ = $1; }
;

type_function_name:
      IDENT
        { $$ = $1; }
    | unreserved_keyword
        { $$ = $1; }
    | type_func_name_keyword
        { $$ = $1; }
;

ColLabel:
      IDENT
        { $$ = $1; }
    | unreserved_keyword
        { $$ = $1; }
    | col_name_keyword
        { $$ = $1; }
    | type_func_name_keyword
        { $$ = $1; }
    | reserved_keyword
        { $$ = $1; }
;

// Keyword categories.
unreserved_keyword:
      ABORT_P { $$ = "abort"; }
    | ABSOLUTE_P { $$ = "absolute"; }
    | ACCESS { $$ = "access"; }
    | ACTION { $$ = "action"; }
    | ADD_P { $$ = "add"; }
    | ADMIN { $$ = "admin"; }
    | AFTER { $$ = "after"; }
    | AGGREGATE { $$ = "aggregate"; }
    | ALSO { $$ = "also"; }
    | ALTER { $$ = "alter"; }
    | ALWAYS { $$ = "always"; }
    | ASENSITIVE { $$ = "asensitive"; }
    | ASSERTION { $$ = "assertion"; }
    | ASSIGNMENT { $$ = "assignment"; }
    | AT { $$ = "at"; }
    | ATOMIC { $$ = "atomic"; }
    | ATTACH { $$ = "attach"; }
    | ATTRIBUTE { $$ = "attribute"; }
    | BACKWARD { $$ = "backward"; }
    | BEFORE { $$ = "before"; }
    | BEGIN_P { $$ = "begin"; }
    | BY { $$ = "by"; }
    | CACHE { $$ = "cache"; }
    | CALL { $$ = "call"; }
    | CALLED { $$ = "called"; }
    | CASCADE { $$ = "cascade"; }
    | CASCADED { $$ = "cascaded"; }
    | CATALOG_P { $$ = "catalog"; }
    | CHAIN { $$ = "chain"; }
    | CHARACTERISTICS { $$ = "characteristics"; }
    | CHECKPOINT { $$ = "checkpoint"; }
    | CLASS { $$ = "class"; }
    | CLOSE { $$ = "close"; }
    | CLUSTER { $$ = "cluster"; }
    | COLUMNS { $$ = "columns"; }
    | COMMENT { $$ = "comment"; }
    | COMMENTS { $$ = "comments"; }
    | COMMIT { $$ = "commit"; }
    | COMMITTED { $$ = "committed"; }
    | COMPRESSION { $$ = "compression"; }
    | CONFIGURATION { $$ = "configuration"; }
    | CONFLICT { $$ = "conflict"; }
    | CONNECTION { $$ = "connection"; }
    | CONSTRAINTS { $$ = "constraints"; }
    | CONTENT_P { $$ = "content"; }
    | CONTINUE_P { $$ = "continue"; }
    | CONVERSION_P { $$ = "conversion"; }
    | COPY { $$ = "copy"; }
    | COST { $$ = "cost"; }
    | CSV { $$ = "csv"; }
    | CUBE { $$ = "cube"; }
    | CURRENT_P { $$ = "current"; }
    | CURSOR { $$ = "cursor"; }
    | CYCLE { $$ = "cycle"; }
    | DATA_P { $$ = "data"; }
    | DATABASE { $$ = "database"; }
    | DAY_P { $$ = "day"; }
    | DEALLOCATE { $$ = "deallocate"; }
    | DECLARE { $$ = "declare"; }
    | DEFAULTS { $$ = "defaults"; }
    | DEFERRED { $$ = "deferred"; }
    | DEFINER { $$ = "definer"; }
    | DELETE_P { $$ = "delete"; }
    | DELIMITER { $$ = "delimiter"; }
    | DELIMITERS { $$ = "delimiters"; }
    | DEPENDS { $$ = "depends"; }
    | DEPTH { $$ = "depth"; }
    | DETACH { $$ = "detach"; }
    | DICTIONARY { $$ = "dictionary"; }
    | DISABLE_P { $$ = "disable"; }
    | DISCARD { $$ = "discard"; }
    | DOCUMENT_P { $$ = "document"; }
    | DOMAIN_P { $$ = "domain"; }
    | DOUBLE_P { $$ = "double"; }
    | DROP { $$ = "drop"; }
    | EACH { $$ = "each"; }
    | ENABLE_P { $$ = "enable"; }
    | ENCODING { $$ = "encoding"; }
    | ENCRYPTED { $$ = "encrypted"; }
    | END_P { $$ = "end"; }
    | ENUM_P { $$ = "enum"; }
    | ESCAPE { $$ = "escape"; }
    | EVENT { $$ = "event"; }
    | EXCLUDE { $$ = "exclude"; }
    | EXCLUDING { $$ = "excluding"; }
    | EXCLUSIVE { $$ = "exclusive"; }
    | EXECUTE { $$ = "execute"; }
    | EXPLAIN { $$ = "explain"; }
    | EXPRESSION { $$ = "expression"; }
    | EXTENSION { $$ = "extension"; }
    | EXTERNAL { $$ = "external"; }
    | FAMILY { $$ = "family"; }
    | FILTER { $$ = "filter"; }
    | FINALIZE { $$ = "finalize"; }
    | FIRST_P { $$ = "first"; }
    | FOLLOWING { $$ = "following"; }
    | FORCE { $$ = "force"; }
    | FORWARD { $$ = "forward"; }
    | FUNCTION { $$ = "function"; }
    | FUNCTIONS { $$ = "functions"; }
    | GENERATED { $$ = "generated"; }
    | GLOBAL { $$ = "global"; }
    | GRANTED { $$ = "granted"; }
    | GROUPS { $$ = "groups"; }
    | HANDLER { $$ = "handler"; }
    | HEADER_P { $$ = "header"; }
    | HOLD { $$ = "hold"; }
    | HOUR_P { $$ = "hour"; }
    | IDENTITY_P { $$ = "identity"; }
    | IF_P { $$ = "if"; }
    | IMMEDIATE { $$ = "immediate"; }
    | IMMUTABLE { $$ = "immutable"; }
    | IMPLICIT_P { $$ = "implicit"; }
    | IMPORT_P { $$ = "import"; }
    | INCLUDE { $$ = "include"; }
    | INCLUDING { $$ = "including"; }
    | INCREMENT { $$ = "increment"; }
    | INDEX { $$ = "index"; }
    | INDEXES { $$ = "indexes"; }
    | INHERIT { $$ = "inherit"; }
    | INHERITS { $$ = "inherits"; }
    | INITIALLY { $$ = "initially"; }
    | INLINE_P { $$ = "inline"; }
    | INPUT_P { $$ = "input"; }
    | INSENSITIVE { $$ = "insensitive"; }
    | INSERT { $$ = "insert"; }
    | INSTEAD { $$ = "instead"; }
    | INVOKER { $$ = "invoker"; }
    | ISOLATION { $$ = "isolation"; }
    | KEY { $$ = "key"; }
    | LABEL { $$ = "label"; }
    | LANGUAGE { $$ = "language"; }
    | LARGE_P { $$ = "large"; }
    | LAST_P { $$ = "last"; }
    | LEAKPROOF { $$ = "leakproof"; }
    | LEVEL { $$ = "level"; }
    | LISTEN { $$ = "listen"; }
    | LOAD { $$ = "load"; }
    | LOCAL { $$ = "local"; }
    | LOCATION { $$ = "location"; }
    | LOCK_P { $$ = "lock"; }
    | LOCKED { $$ = "locked"; }
    | LOGGED { $$ = "logged"; }
    | MAPPING { $$ = "mapping"; }
    | MATCH { $$ = "match"; }
    | MATCHED { $$ = "matched"; }
    | MATERIALIZED { $$ = "materialized"; }
    | MAXVALUE { $$ = "maxvalue"; }
    | MERGE { $$ = "merge"; }
    | METHOD { $$ = "method"; }
    | MINUTE_P { $$ = "minute"; }
    | MINVALUE { $$ = "minvalue"; }
    | MODE { $$ = "mode"; }
    | MONTH_P { $$ = "month"; }
    | MOVE { $$ = "move"; }
    | NAME_P { $$ = "name"; }
    | NAMES { $$ = "names"; }
    | NEW { $$ = "new"; }
    | NEXT { $$ = "next"; }
    | NFC { $$ = "nfc"; }
    | NFD { $$ = "nfd"; }
    | NFKC { $$ = "nfkc"; }
    | NFKD { $$ = "nfkd"; }
    | NO { $$ = "no"; }
    | NOTHING { $$ = "nothing"; }
    | NOTIFY { $$ = "notify"; }
    | NOWAIT { $$ = "nowait"; }
    | NULLS_P { $$ = "nulls"; }
    | OBJECT_P { $$ = "object"; }
    | OF { $$ = "of"; }
    | OFF { $$ = "off"; }
    | OIDS { $$ = "oids"; }
    | OLD { $$ = "old"; }
    | OPERATOR { $$ = "operator"; }
    | OPTION { $$ = "option"; }
    | OPTIONS { $$ = "options"; }
    | ORDINALITY { $$ = "ordinality"; }
    | OTHERS { $$ = "others"; }
    | OVER { $$ = "over"; }
    | OVERRIDING { $$ = "overriding"; }
    | OWNED { $$ = "owned"; }
    | OWNER { $$ = "owner"; }
    | PARALLEL { $$ = "parallel"; }
    | PARAMETER { $$ = "parameter"; }
    | PARSER { $$ = "parser"; }
    | PARTIAL { $$ = "partial"; }
    | PARTITION { $$ = "partition"; }
    | PASSING { $$ = "passing"; }
    | PASSWORD { $$ = "password"; }
    | PLANS { $$ = "plans"; }
    | POLICY { $$ = "policy"; }
    | PRECEDING { $$ = "preceding"; }
    | PREPARE { $$ = "prepare"; }
    | PREPARED { $$ = "prepared"; }
    | PRESERVE { $$ = "preserve"; }
    | PRIOR { $$ = "prior"; }
    | PRIVILEGES { $$ = "privileges"; }
    | PROCEDURAL { $$ = "procedural"; }
    | PROCEDURE { $$ = "procedure"; }
    | PROCEDURES { $$ = "procedures"; }
    | PROGRAM { $$ = "program"; }
    | PUBLICATION { $$ = "publication"; }
    | QUOTE { $$ = "quote"; }
    | RANGE { $$ = "range"; }
    | READ { $$ = "read"; }
    | REASSIGN { $$ = "reassign"; }
    | RECHECK { $$ = "recheck"; }
    | RECURSIVE { $$ = "recursive"; }
    | REF_P { $$ = "ref"; }
    | REFERENCING { $$ = "referencing"; }
    | REFRESH { $$ = "refresh"; }
    | REINDEX { $$ = "reindex"; }
    | RELATIVE_P { $$ = "relative"; }
    | RELEASE { $$ = "release"; }
    | RENAME { $$ = "rename"; }
    | REPEATABLE { $$ = "repeatable"; }
    | REPLACE { $$ = "replace"; }
    | REPLICA { $$ = "replica"; }
    | RESET { $$ = "reset"; }
    | RESTART { $$ = "restart"; }
    | RESTRICT { $$ = "restrict"; }
    | RETURN { $$ = "return"; }
    | RETURNS { $$ = "returns"; }
    | REVOKE { $$ = "revoke"; }
    | ROLE { $$ = "role"; }
    | ROLLBACK { $$ = "rollback"; }
    | ROLLUP { $$ = "rollup"; }
    | ROUTINE { $$ = "routine"; }
    | ROUTINES { $$ = "routines"; }
    | ROWS { $$ = "rows"; }
    | RULE { $$ = "rule"; }
    | SAVEPOINT { $$ = "savepoint"; }
    | SCHEMA { $$ = "schema"; }
    | SCHEMAS { $$ = "schemas"; }
    | SCROLL { $$ = "scroll"; }
    | SEARCH { $$ = "search"; }
    | SECOND_P { $$ = "second"; }
    | SECURITY { $$ = "security"; }
    | SEQUENCE { $$ = "sequence"; }
    | SEQUENCES { $$ = "sequences"; }
    | SERIALIZABLE { $$ = "serializable"; }
    | SERVER { $$ = "server"; }
    | SESSION { $$ = "session"; }
    | SET { $$ = "set"; }
    | SETS { $$ = "sets"; }
    | SHARE { $$ = "share"; }
    | SHOW { $$ = "show"; }
    | SIMPLE { $$ = "simple"; }
    | SKIP { $$ = "skip"; }
    | SNAPSHOT { $$ = "snapshot"; }
    | SQL_P { $$ = "sql"; }
    | STABLE { $$ = "stable"; }
    | STANDALONE_P { $$ = "standalone"; }
    | START { $$ = "start"; }
    | STATEMENT { $$ = "statement"; }
    | STATISTICS { $$ = "statistics"; }
    | STDIN { $$ = "stdin"; }
    | STDOUT { $$ = "stdout"; }
    | STORAGE { $$ = "storage"; }
    | STORED { $$ = "stored"; }
    | STRICT_P { $$ = "strict"; }
    | STRIP_P { $$ = "strip"; }
    | SUBSCRIPTION { $$ = "subscription"; }
    | SUPPORT { $$ = "support"; }
    | SYSID { $$ = "sysid"; }
    | SYSTEM_P { $$ = "system"; }
    | TABLES { $$ = "tables"; }
    | TABLESPACE { $$ = "tablespace"; }
    | TEMP { $$ = "temp"; }
    | TEMPLATE { $$ = "template"; }
    | TEMPORARY { $$ = "temporary"; }
    | TEXT_P { $$ = "text"; }
    | TIES { $$ = "ties"; }
    | TRANSACTION { $$ = "transaction"; }
    | TRANSFORM { $$ = "transform"; }
    | TRIGGER { $$ = "trigger"; }
    | TRUNCATE { $$ = "truncate"; }
    | TRUSTED { $$ = "trusted"; }
    | TYPE_P { $$ = "type"; }
    | TYPES_P { $$ = "types"; }
    | UESCAPE { $$ = "uescape"; }
    | UNBOUNDED { $$ = "unbounded"; }
    | UNCOMMITTED { $$ = "uncommitted"; }
    | UNENCRYPTED { $$ = "unencrypted"; }
    | UNLISTEN { $$ = "unlisten"; }
    | UNLOGGED { $$ = "unlogged"; }
    | UNTIL { $$ = "until"; }
    | UPDATE { $$ = "update"; }
    | UNKNOWN { $$ = "unknown"; }
    | VACUUM { $$ = "vacuum"; }
    | VALID { $$ = "valid"; }
    | VALIDATE { $$ = "validate"; }
    | VALIDATOR { $$ = "validator"; }
    | VALUE_P { $$ = "value"; }
    | VARYING { $$ = "varying"; }
    | VERSION_P { $$ = "version"; }
    | VIEW { $$ = "view"; }
    | VIEWS { $$ = "views"; }
    | VOLATILE { $$ = "volatile"; }
    | WHITESPACE_P { $$ = "whitespace"; }
    | WITHIN { $$ = "within"; }
    | WITHOUT { $$ = "without"; }
    | WORK { $$ = "work"; }
    | WRAPPER { $$ = "wrapper"; }
    | WRITE { $$ = "write"; }
    | XML_P { $$ = "xml"; }
    | YEAR_P { $$ = "year"; }
    | YES_P { $$ = "yes"; }
    | ZONE { $$ = "zone"; }
;

col_name_keyword:
      BETWEEN { $$ = "between"; }
    | BIGINT { $$ = "bigint"; }
    | BIT { $$ = "bit"; }
    | BOOLEAN_P { $$ = "boolean"; }
    | CHAR_P { $$ = "char"; }
    | CHARACTER { $$ = "character"; }
    | COALESCE { $$ = "coalesce"; }
    | DEC { $$ = "dec"; }
    | DECIMAL_P { $$ = "decimal"; }
    | EXISTS { $$ = "exists"; }
    | EXTRACT { $$ = "extract"; }
    | FLOAT_P { $$ = "float"; }
    | GREATEST { $$ = "greatest"; }
    | GROUPING { $$ = "grouping"; }
    | INOUT { $$ = "inout"; }
    | INT_P { $$ = "int"; }
    | INTEGER { $$ = "integer"; }
    | INTERVAL { $$ = "interval"; }
    | LEAST { $$ = "least"; }
    | NATIONAL { $$ = "national"; }
    | NCHAR { $$ = "nchar"; }
    | NONE { $$ = "none"; }
    | NORMALIZE { $$ = "normalize"; }
    | NULLIF { $$ = "nullif"; }
    | NUMERIC { $$ = "numeric"; }
    | OUT_P { $$ = "out"; }
    | OVERLAY { $$ = "overlay"; }
    | POSITION { $$ = "position"; }
    | PRECISION { $$ = "precision"; }
    | REAL { $$ = "real"; }
    | ROW { $$ = "row"; }
    | SETOF { $$ = "setof"; }
    | SMALLINT { $$ = "smallint"; }
    | SUBSTRING { $$ = "substring"; }
    | TIME { $$ = "time"; }
    | TIMESTAMP { $$ = "timestamp"; }
    | TREAT { $$ = "treat"; }
    | TRIM { $$ = "trim"; }
    | VALUES { $$ = "values"; }
    | VARCHAR { $$ = "varchar"; }
    | XMLATTRIBUTES { $$ = "xmlattributes"; }
    | XMLCONCAT { $$ = "xmlconcat"; }
    | XMLELEMENT { $$ = "xmlelement"; }
    | XMLEXISTS { $$ = "xmlexists"; }
    | XMLFOREST { $$ = "xmlforest"; }
    | XMLNAMESPACES { $$ = "xmlnamespaces"; }
    | XMLPARSE { $$ = "xmlparse"; }
    | XMLPI { $$ = "xmlpi"; }
    | XMLROOT { $$ = "xmlroot"; }
    | XMLSERIALIZE { $$ = "xmlserialize"; }
    | XMLTABLE { $$ = "xmltable"; }
;

type_func_name_keyword:
      AUTHORIZATION { $$ = "authorization"; }
    | BINARY { $$ = "binary"; }
    | COLLATION { $$ = "collation"; }
    | CONCURRENTLY { $$ = "concurrently"; }
    | CROSS { $$ = "cross"; }
    | CURRENT_SCHEMA { $$ = "current_schema"; }
    | FREEZE { $$ = "freeze"; }
    | FULL { $$ = "full"; }
    | ILIKE { $$ = "ilike"; }
    | INNER_P { $$ = "inner"; }
    | IS { $$ = "is"; }
    | ISNULL { $$ = "isnull"; }
    | JOIN { $$ = "join"; }
    | LEFT { $$ = "left"; }
    | LIKE { $$ = "like"; }
    | NATURAL { $$ = "natural"; }
    | NOTNULL { $$ = "notnull"; }
    | OUTER_P { $$ = "outer"; }
    | OVERLAPS { $$ = "overlaps"; }
    | RIGHT { $$ = "right"; }
    | SIMILAR { $$ = "similar"; }
    | TABLESAMPLE { $$ = "tablesample"; }
    | VERBOSE { $$ = "verbose"; }
;

reserved_keyword:
      ALL { $$ = "all"; }
    | ANALYSE { $$ = "analyse"; }
    | ANALYZE { $$ = "analyze"; }
    | AND { $$ = "and"; }
    | ANY { $$ = "any"; }
    | ARRAY { $$ = "array"; }
    | AS { $$ = "as"; }
    | ASC { $$ = "asc"; }
    | ASYMMETRIC { $$ = "asymmetric"; }
    | BOTH { $$ = "both"; }
    | CASE { $$ = "case"; }
    | CAST { $$ = "cast"; }
    | CHECK { $$ = "check"; }
    | COLLATE { $$ = "collate"; }
    | COLUMN { $$ = "column"; }
    | CONSTRAINT { $$ = "constraint"; }
    | CREATE { $$ = "create"; }
    | CURRENT_CATALOG { $$ = "current_catalog"; }
    | CURRENT_DATE { $$ = "current_date"; }
    | CURRENT_ROLE { $$ = "current_role"; }
    | CURRENT_TIME { $$ = "current_time"; }
    | CURRENT_TIMESTAMP { $$ = "current_timestamp"; }
    | CURRENT_USER { $$ = "current_user"; }
    | DEFAULT { $$ = "default"; }
    | DEFERRABLE { $$ = "deferrable"; }
    | DISTINCT { $$ = "distinct"; }
    | DO { $$ = "do"; }
    | ELSE { $$ = "else"; }
    | EXCEPT { $$ = "except"; }
    | FALSE_P { $$ = "false"; }
    | FETCH { $$ = "fetch"; }
    | FOR { $$ = "for"; }
    | FOREIGN { $$ = "foreign"; }
    | FROM { $$ = "from"; }
    | GRANT { $$ = "grant"; }
    | GROUP_P { $$ = "group"; }
    | HAVING { $$ = "having"; }
    | IN_P { $$ = "in"; }
    | INITIALLY { $$ = "initially"; }
    | INTERSECT { $$ = "intersect"; }
    | INTO { $$ = "into"; }
    | LATERAL_P { $$ = "lateral"; }
    | LEADING { $$ = "leading"; }
    | LIMIT { $$ = "limit"; }
    | LOCALTIME { $$ = "localtime"; }
    | LOCALTIMESTAMP { $$ = "localtimestamp"; }
    | NOT { $$ = "not"; }
    | NULL_P { $$ = "null"; }
    | OFFSET { $$ = "offset"; }
    | ON { $$ = "on"; }
    | ONLY { $$ = "only"; }
    | OR { $$ = "or"; }
    | ORDER { $$ = "order"; }
    | PLACING { $$ = "placing"; }
    | PRIMARY { $$ = "primary"; }
    | REFERENCES { $$ = "references"; }
    | RETURNING { $$ = "returning"; }
    | SELECT { $$ = "select"; }
    | SESSION_USER { $$ = "session_user"; }
    | SOME { $$ = "some"; }
    | SYMMETRIC { $$ = "symmetric"; }
    | TABLE { $$ = "table"; }
    | THEN { $$ = "then"; }
    | TO { $$ = "to"; }
    | TRAILING { $$ = "trailing"; }
    | TRUE_P { $$ = "true"; }
    | UNION { $$ = "union"; }
    | UNIQUE { $$ = "unique"; }
    | USER { $$ = "user"; }
    | USING { $$ = "using"; }
    | VARIADIC { $$ = "variadic"; }
    | WHEN { $$ = "when"; }
    | WHERE { $$ = "where"; }
    | WINDOW { $$ = "window"; }
    | WITH { $$ = "with"; }
;

%%
// Epilogue.
