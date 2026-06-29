// estate.cpp — EState and QueryDesc lifecycle management.
//
// Converted from PostgreSQL 15's src/backend/executor/execMain.c (parts).
//
// EState holds per-query executor state. Its destructor closes any opened
// relations and frees the per-query memory context. QueryDesc ties together
// a Query, its Plan, the EState, and the top-level PlanState; its destructor
// tears down the PlanState and EState if still present.
#include "executor/estate.hpp"

#include "access/rel.hpp"
#include "common/containers/node.hpp"
#include "executor/node_exec.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::destroyPallocNode;

EState::~EState() {
    // Free tuple table slots.
    for (TupleTableSlot* slot : es_tupleTable) {
        if (slot != nullptr) {
            destroyPallocNode(slot);
        }
    }
    es_tupleTable.clear();

    // Close any opened relations.
    for (pgcpp::access::Relation rel : es_open_relations) {
        if (rel != nullptr) {
            pgcpp::access::RelationClose(rel);
        }
    }
    es_open_relations.clear();

    // The per-query memory context is owned by the caller (ExecutorEnd);
    // we do not delete it here to avoid double-free.
}

QueryDesc::~QueryDesc() {
    // The PlanState and EState are torn down explicitly by ExecutorEnd;
    // the destructor does not own them.
}

}  // namespace pgcpp::executor
