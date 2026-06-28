// node_hash.h — Hash table build node state (inner child of HashJoin).
#pragma once

#include <unordered_map>
#include <vector>

#include "pgcpp/executor/node_exec.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::executor {

// HashEntry — a tuple stored in the hash table.
struct HashEntry {
    TupleTableSlot* slot;
    pgcpp::types::Datum hashkey;
    bool keynull;
};

// HashTable — the hash table built by the Hash node.
struct HashTable {
    // Key: hash of the join key values. Value: list of matching tuples.
    std::unordered_multimap<uint64_t, HashEntry> buckets;

    void Insert(uint64_t hash, TupleTableSlot* slot, pgcpp::types::Datum keyval, bool keynull);
    void Clear();
};

class HashState : public PlanState {
public:
    HashState(Plan* p, EState* s) : PlanState(p, s) {}

    HashTable hashtable;
    std::vector<pgcpp::parser::Node*> hashkeys;  // hash key expressions

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor
