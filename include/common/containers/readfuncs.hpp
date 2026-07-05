// readfuncs.h — node tree deserialization (string → Node).
//
// Converted from PostgreSQL 15's src/backend/nodes/readfuncs.c.
//
// Deserializes a PG-format S-expression string back into a node tree.
// This is the inverse of outfuncs (nodeToString).
//
// Format:
//   (TAG :fieldname value :fieldname value ...)
//
// See outfuncs.hpp for format details.
#pragma once

namespace pgcpp::nodes {
class Node;
}  // namespace pgcpp::nodes

namespace pgcpp::nodes {

// stringToNode — deserialize a PG-format string into a node tree.
// Returns a palloc'd node (caller must pfree via destroyPallocNode or
// let the MemoryContext handle cleanup). Returns nullptr for empty/null
// input.
Node* stringToNode(const char* str);

}  // namespace pgcpp::nodes
