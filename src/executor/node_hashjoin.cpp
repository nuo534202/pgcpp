// node_hashjoin.cpp — Hash join implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeHashjoin.c.
//
// Implements hybrid hash join with batch spilling (P1-4). The join has
// multiple phases:
//   1. Build: consume all tuples from the inner (right) child via the
//      Hash node, inserting each into a hash table keyed by the hash
//      join clauses. When the in-memory hash table exceeds work_mem,
//      tuples are distributed across multiple batches: batch 0 stays
//      in memory, batches 1..N-1 spill to tuplestores on disk.
//   2. Probe (batch 0): for each tuple from the outer (left) child,
//      compute the hash of the join keys and look up matching tuples
//      in the hash table. Outer tuples that hash to non-zero batches
//      are spilled to outer batch tuplestores.
//   3. Scan unmatched (RIGHT/FULL only): scan the hash table for inner
//      tuples that were never matched, and emit them with NULL outer
//      columns.
//   4. Next batch: load the next spilled inner batch into the hash
//      table and probe with the corresponding outer batch.
//
// Supports INNER, LEFT, RIGHT, FULL, SEMI, and ANTI joins.
//   - INNER: emit matching pairs only.
//   - LEFT:  emit matching pairs; unmatched outer rows get NULL inner.
//   - RIGHT: emit matching pairs; unmatched inner rows get NULL outer.
//   - FULL:  LEFT + RIGHT combined.
//   - SEMI:  emit outer row on first match; skip rest of bucket.
//   - ANTI:  emit outer row if NO match found in the bucket.
#include "executor/node_hashjoin.hpp"

#include <new>

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/node_hash.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

using pgcpp::catalog::Oid;
using pgcpp::nodes::destroyPallocNode;
using pgcpp::parser::JoinType;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Var;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE_DATA;

namespace {

// Compute a hash value for a Datum of the given type.
uint64_t HashDatum(Datum val, Oid typid) {
    switch (typid) {
        case kInt4Oid:
            return static_cast<uint64_t>(DatumGetInt32(val));
        case kInt8Oid:
            return static_cast<uint64_t>(DatumGetInt64(val));
        case kFloat8Oid: {
            double d = DatumGetFloat8(val);
            uint64_t bits;
            std::memcpy(&bits, &d, sizeof(bits));
            return bits;
        }
        case kTextOid: {
            const char* p = DatumGetTextP(val);
            int len = VARSIZE_DATA(p);
            const char* data = VARDATA(p);
            uint64_t h = 14695981039346656037ULL;  // FNV offset basis
            for (int i = 0; i < len; i++) {
                h ^= static_cast<uint64_t>(static_cast<unsigned char>(data[i]));
                h *= 1099511628211ULL;  // FNV prime
            }
            return h;
        }
        default:
            return static_cast<uint64_t>(val);
    }
}

// Determine the type of a hash key expression by looking at the Var node.
Oid GetExprType(Node* expr, TupleTableSlot* /*ref_slot*/) {
    if (expr == nullptr)
        return kInt4Oid;
    if (expr->GetTag() == pgcpp::nodes::NodeTag::kVar) {
        auto* var = static_cast<Var*>(expr);
        return var->vartype;
    }
    return kInt4Oid;
}

}  // namespace

void HashJoinState::ExecInit() {
    auto* hjplan = static_cast<HashJoin*>(plan);
    hj_jointype = hjplan->jointype;
    hj_hashclauses = hjplan->hashclauses;

    // Extract left (outer) and right (inner) hash key expressions from each
    // hash clause OpExpr.
    for (Node* clause : hj_hashclauses) {
        if (clause != nullptr && clause->GetTag() == pgcpp::nodes::NodeTag::kOpExpr) {
            auto* op = static_cast<OpExpr*>(clause);
            if (op->args.size() >= 2) {
                hj_outer_hashkeys.push_back(op->args[0]);
                hj_inner_hashkeys.push_back(op->args[1]);
            }
        }
    }

    // The right child should be a Hash node.
    hj_HashState = dynamic_cast<HashState*>(rightps);
    if (hj_HashState == nullptr && rightps != nullptr) {
        hj_HashState = reinterpret_cast<HashState*>(rightps);
    }

    // Create the result slot from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(hjplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();

    // Create outer tuple slot.
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        hj_OuterTupleSlot = TupleTableSlot::Make(leftps->ps_ResultTupleSlot->tts_tupleDescriptor);
        state->es_tupleTable.push_back(hj_OuterTupleSlot);
        ps_ExprContext->ecxt_outertuple = hj_OuterTupleSlot;
    }

    // Set up the inner tuple slot from the Hash child.
    if (hj_HashState != nullptr && hj_HashState->leftps != nullptr &&
        hj_HashState->leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_innertuple =
            TupleTableSlot::Make(hj_HashState->leftps->ps_ResultTupleSlot->tts_tupleDescriptor);
        state->es_tupleTable.push_back(ps_ExprContext->ecxt_innertuple);
    }

    // Provide the outer tuple descriptor to the Hash state so it can create
    // outer batch tuplestores with the correct schema.
    if (hj_HashState != nullptr && hj_OuterTupleSlot != nullptr) {
        hj_HashState->outer_tupdesc = hj_OuterTupleSlot->tts_tupleDescriptor;
    }

    hj_phase = Phase::kBuildHashTable;
    hj_NeedNewOuter = true;
    hj_hasBucket = false;
    hj_cur_batch = 0;
    hj_scanStarted = false;
}

void HashJoinState::BuildHashTable() {
    TupleTableSlot* ref_slot = (hj_HashState != nullptr && hj_HashState->leftps != nullptr)
                                   ? hj_HashState->leftps->ps_ResultTupleSlot
                                   : nullptr;

    // Determine hash key types from the inner hash keys.
    std::vector<Oid> key_types;
    for (Node* key : hj_inner_hashkeys) {
        key_types.push_back(GetExprType(key, ref_slot));
    }

    for (;;) {
        TupleTableSlot* inner = nullptr;
        if (hj_HashState != nullptr) {
            inner = hj_HashState->ExecProcNode();
        }
        if (inner == nullptr)
            break;

        // Compute hash of the inner join keys.
        ps_ExprContext->ecxt_scantuple = inner;
        uint64_t hash = 0;
        for (size_t i = 0; i < hj_inner_hashkeys.size(); i++) {
            bool isnull = false;
            Datum val = ExecEvalExpr(hj_inner_hashkeys[i], ps_ExprContext, &isnull);
            if (!isnull) {
                hash ^= HashDatum(val, key_types[i]) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            }
        }

        // Copy the inner tuple into a slot owned by the hash table.
        TupleTableSlot* ht_slot = TupleTableSlot::Make(inner->tts_tupleDescriptor);
        ht_slot->StoreVirtual(inner->tts_values, inner->tts_isnull);

        // Determine which batch this tuple belongs to.
        int batch = hj_HashState->BatchOfHash(hash);
        if (batch == 0) {
            // Insert into in-memory hash table.
            hj_HashState->hashtable.Insert(hash, ht_slot, 0, false);
            hj_HashState->mem_used += sizeof(HashEntry) + sizeof(TupleTableSlot) +
                                      inner->Natts() * (sizeof(Datum) + sizeof(bool));

            // If the hash table exceeds work_mem, expand to multiple batches.
            if (hj_HashState->num_batches == 1 && hj_HashState->mem_used > hj_HashState->work_mem) {
                // Choose new_num_batches as next power of 2 >= mem_used/work_mem.
                size_t ratio = hj_HashState->mem_used / hj_HashState->work_mem;
                if (ratio < 1)
                    ratio = 1;
                int new_batches = 2;
                while (new_batches < static_cast<int>(ratio) && new_batches < 1024) {
                    new_batches *= 2;
                }
                hj_HashState->ExpandBatches(new_batches);
            }
        } else {
            // Spill to the appropriate inner batch tuplestore.
            hj_HashState->inner_batches[batch - 1]->PutTuple(ht_slot);
            destroyPallocNode(ht_slot);
        }
    }
}

uint64_t HashJoinState::HashOuterKeys(bool* any_null) {
    *any_null = false;
    uint64_t hash = 0;
    ps_ExprContext->ecxt_outertuple = hj_OuterTupleSlot;
    ps_ExprContext->ecxt_scantuple = hj_OuterTupleSlot;

    std::vector<Oid> key_types;
    for (Node* key : hj_outer_hashkeys) {
        key_types.push_back(GetExprType(key, hj_OuterTupleSlot));
    }

    for (size_t i = 0; i < hj_outer_hashkeys.size(); i++) {
        bool isnull = false;
        Datum val = ExecEvalExpr(hj_outer_hashkeys[i], ps_ExprContext, &isnull);
        if (isnull) {
            *any_null = true;
            return 0;
        }
        hash ^= HashDatum(val, key_types[i]) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    }
    return hash;
}

uint64_t HashJoinState::HashInnerKeys(TupleTableSlot* slot, bool* any_null) {
    *any_null = false;
    uint64_t hash = 0;
    ps_ExprContext->ecxt_scantuple = slot;

    std::vector<Oid> key_types;
    for (Node* key : hj_inner_hashkeys) {
        key_types.push_back(GetExprType(key, slot));
    }

    for (size_t i = 0; i < hj_inner_hashkeys.size(); i++) {
        bool isnull = false;
        Datum val = ExecEvalExpr(hj_inner_hashkeys[i], ps_ExprContext, &isnull);
        if (isnull) {
            *any_null = true;
            return 0;
        }
        hash ^= HashDatum(val, key_types[i]) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    }
    return hash;
}

TupleTableSlot* HashJoinState::EmitNullInnerRow() {
    // Emit the outer tuple with NULL inner columns (LEFT/FULL unmatched outer).
    ps_ExprContext->ecxt_outertuple = hj_OuterTupleSlot;
    ps_ExprContext->ecxt_scantuple = hj_OuterTupleSlot;
    for (int i = 0; i < ps_ExprContext->ecxt_innertuple->Natts(); i++) {
        ps_ExprContext->ecxt_innertuple->tts_values[i] = 0;
        ps_ExprContext->ecxt_innertuple->tts_isnull[i] = true;
    }
    ps_ExprContext->ecxt_innertuple->tts_nvalid = true;
    ps_ExprContext->ecxt_innertuple->tts_isempty = false;
    ResetExprContext(ps_ExprContext);
    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
    return ps_ResultTupleSlot;
}

TupleTableSlot* HashJoinState::EmitNullOuterRow(TupleTableSlot* inner) {
    // Emit the inner tuple with NULL outer columns (RIGHT/FULL unmatched inner).
    ps_ExprContext->ecxt_innertuple->StoreVirtual(inner->tts_values, inner->tts_isnull);
    for (int i = 0; i < hj_OuterTupleSlot->Natts(); i++) {
        hj_OuterTupleSlot->tts_values[i] = 0;
        hj_OuterTupleSlot->tts_isnull[i] = true;
    }
    hj_OuterTupleSlot->tts_nvalid = true;
    hj_OuterTupleSlot->tts_isempty = false;
    ps_ExprContext->ecxt_outertuple = hj_OuterTupleSlot;
    ps_ExprContext->ecxt_scantuple = hj_OuterTupleSlot;
    ResetExprContext(ps_ExprContext);
    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
    return ps_ResultTupleSlot;
}

TupleTableSlot* HashJoinState::ScanUnmatched() {
    auto& buckets = hj_HashState->hashtable.buckets;
    if (!hj_scanStarted) {
        hj_scanIter = buckets.begin();
        hj_scanStarted = true;
    }
    while (hj_scanIter != buckets.end()) {
        if (!hj_scanIter->second.matched && hj_scanIter->second.slot != nullptr) {
            TupleTableSlot* inner = hj_scanIter->second.slot;
            ++hj_scanIter;
            return EmitNullOuterRow(inner);
        }
        ++hj_scanIter;
    }
    return nullptr;  // scan complete
}

bool HashJoinState::LoadNextBatch() {
    hj_cur_batch++;
    if (hj_cur_batch >= hj_HashState->num_batches) {
        return false;  // no more batches
    }

    // Clear the in-memory hash table (frees slots from previous batch).
    hj_HashState->ClearHashTable();

    // Load all tuples from inner_batches[cur_batch-1] into the hash table.
    auto& inner_store = hj_HashState->inner_batches[hj_cur_batch - 1];
    if (inner_store == nullptr) {
        return true;  // empty batch, but still need to probe outer
    }
    inner_store->Rewind();

    TupleTableSlot* ref_slot =
        (hj_HashState->leftps != nullptr) ? hj_HashState->leftps->ps_ResultTupleSlot : nullptr;
    std::vector<Oid> key_types;
    for (Node* key : hj_inner_hashkeys) {
        key_types.push_back(GetExprType(key, ref_slot));
    }

    for (;;) {
        TupleTableSlot* inner = inner_store->GetTuple();
        if (inner == nullptr)
            break;

        // Compute hash of the inner join keys.
        bool any_null = false;
        uint64_t hash = HashInnerKeys(inner, &any_null);
        if (any_null)
            continue;  // NULL keys never match

        // Copy the inner tuple into a slot owned by the hash table.
        TupleTableSlot* ht_slot = TupleTableSlot::Make(inner->tts_tupleDescriptor);
        ht_slot->StoreVirtual(inner->tts_values, inner->tts_isnull);
        hj_HashState->hashtable.Insert(hash, ht_slot, 0, false);
    }

    // Rewind the outer batch tuplestore for probing.
    auto& outer_store = hj_HashState->outer_batches[hj_cur_batch - 1];
    if (outer_store != nullptr) {
        outer_store->Rewind();
    }

    hj_NeedNewOuter = true;
    hj_hasBucket = false;
    hj_scanStarted = false;
    return true;
}

TupleTableSlot* HashJoinState::ExecProcNode() {
    for (;;) {
        if (hj_phase == Phase::kBuildHashTable) {
            BuildHashTable();
            hj_phase = Phase::kProbeHashTable;
            hj_NeedNewOuter = true;
            hj_cur_batch = 0;
            continue;
        }

        if (hj_phase == Phase::kProbeHashTable) {
            // --- Phase A: process existing bucket entries ---
            if (hj_hasBucket && hj_curBucket != hj_curBucketEnd) {
                TupleTableSlot* inner = hj_curBucket->second.slot;
                hj_curBucket->second.matched = true;

                ps_ExprContext->ecxt_innertuple->StoreVirtual(inner->tts_values, inner->tts_isnull);
                ps_ExprContext->ecxt_outertuple = hj_OuterTupleSlot;
                ps_ExprContext->ecxt_scantuple = hj_OuterTupleSlot;
                ++hj_curBucket;

                // SEMI join: emit outer on first match, skip rest of bucket.
                if (hj_jointype == JoinType::kSemi) {
                    ResetExprContext(ps_ExprContext);
                    if (plan->qual == nullptr || ExecQual(plan->qual, ps_ExprContext)) {
                        hj_MatchedOuter = true;
                        hj_hasBucket = false;
                        hj_NeedNewOuter = true;
                        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
                        return ps_ResultTupleSlot;
                    }
                    continue;  // qual didn't pass, try next
                }

                // ANTI join: if match found, skip this outer entirely.
                if (hj_jointype == JoinType::kAnti) {
                    ResetExprContext(ps_ExprContext);
                    if (plan->qual == nullptr || ExecQual(plan->qual, ps_ExprContext)) {
                        hj_MatchedOuter = true;
                        hj_hasBucket = false;
                        hj_NeedNewOuter = true;
                        continue;  // skip to next outer
                    }
                    continue;  // qual didn't pass, try next
                }

                // INNER/LEFT/RIGHT/FULL: emit match if qual passes.
                ResetExprContext(ps_ExprContext);
                if (plan->qual == nullptr || ExecQual(plan->qual, ps_ExprContext)) {
                    hj_MatchedOuter = true;
                    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
                    return ps_ResultTupleSlot;
                }
                continue;  // qual didn't pass, try next
            }

            // Bucket exhausted — handle post-bucket logic for the current outer.
            if (hj_hasBucket) {
                hj_hasBucket = false;
                if (!hj_NeedNewOuter) {
                    hj_NeedNewOuter = true;
                    // ANTI: emit outer if no match was found.
                    if (hj_jointype == JoinType::kAnti && !hj_MatchedOuter) {
                        return EmitNullInnerRow();
                    }
                    // LEFT/FULL: emit NULL-padded row if no match was found.
                    if ((hj_jointype == JoinType::kLeft || hj_jointype == JoinType::kFull) &&
                        !hj_MatchedOuter) {
                        return EmitNullInnerRow();
                    }
                    // SEMI/INNER/RIGHT: no match → move to next outer.
                }
            }

            // --- Phase B: fetch next outer tuple ---
            if (!hj_NeedNewOuter) {
                // Should not reach here, but guard against infinite loop.
                hj_NeedNewOuter = true;
            }

            TupleTableSlot* outer = nullptr;
            if (hj_cur_batch == 0) {
                // Batch 0: read outer from left child.
                if (leftps != nullptr) {
                    outer = leftps->ExecProcNode();
                }
            } else {
                // Batch > 0: read outer from the batch tuplestore.
                auto& outer_store = hj_HashState->outer_batches[hj_cur_batch - 1];
                if (outer_store != nullptr) {
                    outer = outer_store->GetTuple();
                }
            }

            if (outer == nullptr) {
                // No more outer tuples for this batch.
                if (hj_jointype == JoinType::kRight || hj_jointype == JoinType::kFull) {
                    hj_phase = Phase::kScanUnmatched;
                    hj_scanStarted = false;
                    continue;
                }
                // Move to next batch.
                if (LoadNextBatch()) {
                    hj_phase = Phase::kProbeHashTable;
                    continue;
                }
                hj_phase = Phase::kDone;
                continue;
            }

            // Store the outer tuple.
            hj_OuterTupleSlot->StoreVirtual(outer->tts_values, outer->tts_isnull);
            hj_NeedNewOuter = false;
            hj_MatchedOuter = false;

            // Compute hash of the outer join keys.
            bool any_null = false;
            uint64_t hash = HashOuterKeys(&any_null);

            if (any_null) {
                // NULL join key: no matches possible.
                if (hj_jointype == JoinType::kLeft || hj_jointype == JoinType::kFull) {
                    hj_NeedNewOuter = true;
                    return EmitNullInnerRow();
                }
                if (hj_jointype == JoinType::kAnti) {
                    hj_NeedNewOuter = true;
                    return EmitNullInnerRow();
                }
                // INNER/RIGHT/SEMI: skip this outer.
                hj_NeedNewOuter = true;
                continue;
            }

            // Determine which batch this outer tuple belongs to.
            int batch = hj_HashState->BatchOfHash(hash);
            if (batch != hj_cur_batch) {
                // Spill to the appropriate outer batch tuplestore.
                if (batch > 0) {
                    hj_HashState->outer_batches[batch - 1]->PutTuple(hj_OuterTupleSlot);
                }
                hj_NeedNewOuter = true;
                continue;
            }

            // Look up the hash bucket.
            auto& buckets = hj_HashState->hashtable.buckets;
            auto range = buckets.equal_range(hash);
            hj_curBucket = range.first;
            hj_curBucketEnd = range.second;
            hj_hasBucket = true;
            // Loop back to Phase A to process the bucket.
            continue;
        }

        if (hj_phase == Phase::kScanUnmatched) {
            // RIGHT/FULL: scan hash table for unmatched inner tuples.
            TupleTableSlot* result = ScanUnmatched();
            if (result != nullptr) {
                return result;
            }
            // Scan complete — move to next batch.
            if (LoadNextBatch()) {
                hj_phase = Phase::kProbeHashTable;
                continue;
            }
            hj_phase = Phase::kDone;
            continue;
        }

        return nullptr;  // kDone
    }
}

void HashJoinState::ExecEnd() {
    // The HashState's ExecEnd (called via ExecEndNode recursion) handles
    // freeing the hash table slots and batch tuplestores. We just need to
    // reset our own state.
    hj_phase = Phase::kDone;
    hj_hasBucket = false;
    hj_NeedNewOuter = true;

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void HashJoinState::ExecReScan() {
    if (hj_HashState != nullptr) {
        hj_HashState->ResetBatches();
    }
    hj_phase = Phase::kBuildHashTable;
    hj_NeedNewOuter = true;
    hj_hasBucket = false;
    hj_cur_batch = 0;
    hj_scanStarted = false;
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
    if (hj_HashState != nullptr) {
        hj_HashState->ExecReScan();
    }
}

}  // namespace pgcpp::executor
