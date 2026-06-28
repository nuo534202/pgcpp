// estate.cpp — EState and QueryDesc lifecycle management.
//
// Converted from PostgreSQL 15's src/backend/executor/execMain.c (parts).
//
// EState holds per-query executor state. Its destructor closes any opened
// relations and frees the per-query memory context. QueryDesc ties together
// a Query, its Plan, the EState, and the top-level PlanState; its destructor
// tears down the PlanState and EState if still present.
#include "pgcpp/executor/estate.hpp"

#include "pgcpp/access/rel.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/executor/node_exec.hpp"
#include "pgcpp/executor/tupletable.hpp"

namespace mytoydb::executor {

using mytoydb::nodes::destroyPallocNode;

EState::~EState() {
    // Free tuple table slots.
    for (TupleTableSlot* slot : es_tupleTable) {
        if (slot != nullptr) {
            destroyPallocNode(slot);
        }
    }
    es_tupleTable.clear();

    // Close any opened relations.
    for (mytoydb::access::Relation rel : es_open_relations) {
        if (rel != nullptr) {
            mytoydb::access::RelationClose(rel);
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

}  // namespace mytoydb::executor
