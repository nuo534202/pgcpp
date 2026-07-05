// outfuncs.h — node tree serialization (Node → string).
//
// Converted from PostgreSQL 15's src/backend/nodes/outfuncs.c.
//
// Serializes node trees to a Lisp-like S-expression format compatible with
// PostgreSQL's nodeToString output:
//
//   (TAG :fieldname value :fieldname value ...)
//
// Nested nodes are parenthesized:
//   (VAR :varno 1 :varattno 2 :vartype 23 :location -1)
//
// Lists of nodes are space-separated within parentheses:
//   (:args (CONST :constvalue 42) (VAR :varno 1))
//
// Null pointers are serialized as <>.
//
// This is the pgcpp equivalent of PG's outfuncs.c. Unlike PG (which uses
// centralized switch statements), pgcpp already implements Clone/Equals as
// virtual methods on each node class. For Out/Read, we use a centralized
// dispatch approach to avoid modifying every node header with virtual
// methods — serialization is a cross-cutting concern that doesn't belong
// to individual node types.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pgcpp::containers {
class StringInfo;
}  // namespace pgcpp::containers

namespace pgcpp::nodes {
class Node;
}  // namespace pgcpp::nodes

namespace pgcpp::nodes {

// nodeToString — serialize a node tree to a PG-format string.
// Returns a palloc'd C string (caller must pfree). Returns nullptr for
// nullptr input.
char* nodeToString(const Node* node);

// nodeToStdString — C++ convenience wrapper returning std::string.
std::string nodeToStdString(const Node* node);

// --- Helper functions (used by per-type _out functions) ---

// Write a field label and integer value: " :label value"
void outInt(pgcpp::containers::StringInfo* buf, const char* label, int value);
void outInt64(pgcpp::containers::StringInfo* buf, const char* label, int64_t value);
void outBool(pgcpp::containers::StringInfo* buf, const char* label, bool value);
void outOid(pgcpp::containers::StringInfo* buf, const char* label, unsigned int value);
void outString(pgcpp::containers::StringInfo* buf, const char* label, std::string_view value);
// Write a field label and a nested node: " :label (NESTED ...)"
void outNodeField(pgcpp::containers::StringInfo* buf, const char* label, const Node* value);
// Write a field label and a list of nodes: " :label (item1 item2 ...)"
void outNodeList(pgcpp::containers::StringInfo* buf, const char* label,
                 const std::vector<Node*>& list);
// Write a char field
void outChar(pgcpp::containers::StringInfo* buf, const char* label, char value);

}  // namespace pgcpp::nodes
