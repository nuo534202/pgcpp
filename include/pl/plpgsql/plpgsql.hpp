// plpgsql.hpp — PL/pgSQL handler public API.
//
// Converted from PostgreSQL 15's src/pl/plpgsql/src/plpgsql.h (subset).
//
// pgcpp implements a focused subset of PL/pgSQL:
//   - DECLARE section with variable declarations (int4/int8/float8/text/bool)
//   - BEGIN ... END body with optional EXCEPTION clause (skipped)
//   - Assignment: var := expr
//   - IF cond THEN ... [ELSIF cond THEN ...] [ELSE ...] END IF
//   - [<<label>>] LOOP ... END LOOP [label]  (with EXIT/CONTINUE)
//   - WHILE cond LOOP ... END LOOP
//   - FOR i IN [REVERSE] low..high LOOP ... END LOOP
//   - RETURN expr | RETURN
//   - EXIT [label] [WHEN expr]
//   - CONTINUE [label] [WHEN expr]
//   - Expressions: literals, variable refs, $1..$N args, +, -, *, /,
//     comparisons, AND/OR/NOT, function calls (via fmgr)
//
// Out of scope (documented): SQL queries inside PL (PERFORM/SELECT INTO),
// record/row types, triggers, dynamic SQL (EXECUTE), cursors, exception
// blocks, GUC handling, polymorphic types, variadic, named-arg calls.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "pl/pl_handler.hpp"
#include "types/datum.hpp"

namespace pgcpp::pl::plpgsql {

// Register the PL/pgSQL handler with the global PL handler registry.
// Idempotent; safe to call multiple times (subsequent calls replace the
// prior registration). Called once at server startup.
void RegisterPlPgsqlHandler();

// --- AST node types ---
//
// Each statement and expression is represented by a node owned by a
// std::unique_ptr so the AST is freed automatically when the function
// compilation context goes away.

enum class PlNodeKind {
    kBlock,
    kVarDecl,
    kAssignment,
    kIfStmt,
    kLoopStmt,
    kWhileStmt,
    kForStmt,
    kReturnStmt,
    kExitStmt,
    kContinueStmt,
    kNullStmt,
    kIntegerLit,
    kFloatLit,
    kStringLit,
    kBoolLit,
    kNullLit,
    kVarRef,
    kUnaryOp,
    kBinaryOp,
    kFunctionCall,
};

// Type OID set we support for variables and return values.
struct PlType {
    pgcpp::catalog::Oid oid = 0;
    bool is_int = false;    // int4/int8
    bool is_float = false;  // float8
    bool is_text = false;   // text
    bool is_bool = false;   // bool
};

// Forward declaration of the base node.
struct PlNode;

using PlNodePtr = std::unique_ptr<PlNode>;

// Base AST node.
struct PlNode {
    PlNodeKind kind;
    explicit PlNode(PlNodeKind k) : kind(k) {}
    virtual ~PlNode() = default;
};

// Block: DECLARE ... BEGIN ... END
struct PlBlock : PlNode {
    std::vector<PlNodePtr> declarations;
    std::vector<PlNodePtr> body;
    std::string label;  // optional <<label>>
    PlBlock() : PlNode(PlNodeKind::kBlock) {}
};

// Variable declaration: name type [:= expr | DEFAULT expr]
struct PlVarDecl : PlNode {
    std::string name;
    PlType type;
    PlNodePtr default_expr;  // may be null
    PlVarDecl() : PlNode(PlNodeKind::kVarDecl) {}
};

// Assignment: target := expr
struct PlAssignment : PlNode {
    std::string target;
    PlNodePtr value;
    PlAssignment() : PlNode(PlNodeKind::kAssignment) {}
};

// IF statement.
struct PlIfStmt : PlNode {
    struct Branch {
        PlNodePtr cond;
        std::vector<PlNodePtr> body;
    };
    std::vector<Branch> branches;      // first is IF, rest are ELSIF
    std::vector<PlNodePtr> else_body;  // empty if no ELSE
    PlIfStmt() : PlNode(PlNodeKind::kIfStmt) {}
};

// Unconditional LOOP.
struct PlLoopStmt : PlNode {
    std::vector<PlNodePtr> body;
    std::string label;
    PlLoopStmt() : PlNode(PlNodeKind::kLoopStmt) {}
};

// WHILE cond LOOP.
struct PlWhileStmt : PlNode {
    PlNodePtr cond;
    std::vector<PlNodePtr> body;
    std::string label;
    PlWhileStmt() : PlNode(PlNodeKind::kWhileStmt) {}
};

// FOR i IN [REVERSE] low..high LOOP.
struct PlForStmt : PlNode {
    std::string var_name;
    PlNodePtr low;
    PlNodePtr high;
    bool reverse = false;
    std::vector<PlNodePtr> body;
    std::string label;
    PlForStmt() : PlNode(PlNodeKind::kForStmt) {}
};

// RETURN expr | RETURN
struct PlReturnStmt : PlNode {
    PlNodePtr value;  // may be null (procedure return)
    PlReturnStmt() : PlNode(PlNodeKind::kReturnStmt) {}
};

// EXIT [label] [WHEN expr]
struct PlExitStmt : PlNode {
    std::string label;
    PlNodePtr when;            // may be null
    bool is_continue = false;  // false=EXIT, true=CONTINUE
    PlExitStmt() : PlNode(PlNodeKind::kExitStmt) {}
};

// CONTINUE — represented as PlExitStmt with is_continue=true, but we keep
// a separate node kind so consumers can switch on it cleanly.
struct PlContinueStmt : PlNode {
    std::string label;
    PlNodePtr when;
    PlContinueStmt() : PlNode(PlNodeKind::kContinueStmt) {}
};

// Null statement (no-op, used for empty bodies / NULL keyword).
struct PlNullStmt : PlNode {
    PlNullStmt() : PlNode(PlNodeKind::kNullStmt) {}
};

// Integer literal.
struct PlIntegerLit : PlNode {
    int64_t value = 0;
    PlIntegerLit() : PlNode(PlNodeKind::kIntegerLit) {}
};

// Float literal (stored as double; resolved to float8 at eval time).
struct PlFloatLit : PlNode {
    double value = 0.0;
    PlFloatLit() : PlNode(PlNodeKind::kFloatLit) {}
};

// String literal.
struct PlStringLit : PlNode {
    std::string value;
    PlStringLit() : PlNode(PlNodeKind::kStringLit) {}
};

// Boolean literal (TRUE / FALSE).
struct PlBoolLit : PlNode {
    bool value = false;
    PlBoolLit() : PlNode(PlNodeKind::kBoolLit) {}
};

// NULL literal.
struct PlNullLit : PlNode {
    PlNullLit() : PlNode(PlNodeKind::kNullLit) {}
};

// Variable reference.
struct PlVarRef : PlNode {
    std::string name;
    PlVarRef() : PlNode(PlNodeKind::kVarRef) {}
};

// Unary op: NOT, -
struct PlUnaryOp : PlNode {
    std::string op;  // "NOT", "-"
    PlNodePtr operand;
    PlUnaryOp() : PlNode(PlNodeKind::kUnaryOp) {}
};

// Binary op: +, -, *, /, =, <>, !=, <, <=, >, >=, AND, OR
struct PlBinaryOp : PlNode {
    std::string op;
    PlNodePtr left;
    PlNodePtr right;
    PlBinaryOp() : PlNode(PlNodeKind::kBinaryOp) {}
};

// Function call: name(arg1, arg2, ...)
struct PlFunctionCall : PlNode {
    std::string name;
    std::vector<PlNodePtr> args;
    PlFunctionCall() : PlNode(PlNodeKind::kFunctionCall) {}
};

// Parse a PL/pgSQL source string into an AST. Returns the root PlBlock on
// success. Throws PgException (via ereport) on syntax error.
PlBlock* ParsePlPgsql(const std::string& src);

}  // namespace pgcpp::pl::plpgsql
