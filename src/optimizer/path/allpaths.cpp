// allpaths.cpp — Path generation for base relations.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/allpaths.c.
//
// Creates candidate access paths for each base relation in the query.
// For MyToyDB, this generates SeqScan paths (and optionally IndexPath when
// suitable indexes exist). The cheapest path is selected for each relation.
#include "mytoydb/catalog/catalog.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/optimizer/cost.h"
#include "mytoydb/optimizer/path.h"
#include "mytoydb/optimizer/planner.h"
#include "mytoydb/parser/parsenodes.h"
#include "mytoydb/parser/primnodes.h"

namespace mytoydb::optimizer {
using mytoydb::nodes::makePallocNode;

using mytoydb::catalog::GetCatalog;
using mytoydb::memory::palloc;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Node;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RTEKind;

// Create a SeqScan path for a base relation.
// Estimates cost based on relation size (pages, tuples).
static SeqScanPath* CreateSeqScanPath(RelOptInfo* rel) {
    auto* path = makePallocNode<SeqScanPath>();
    path->relid = rel->relid;

    // Get relation size from catalog (heuristic: assume 1000 rows, 10 pages
    // if no statistics available).
    int pages = 10;
    int tuples = 1000;
    // TODO: look up pg_class.reltuples/relpages when available.

    CostSeqScan(path, pages, tuples);
    return path;
}

// Set up the path list for a base relation.
// Creates SeqScan path and, if indexes exist, IndexPath candidates.
static void SetBaseRelPathlist(PlannerInfo* root, RelOptInfo* rel) {
    // Always create a SeqScan path.
    SeqScanPath* seqpath = CreateSeqScanPath(rel);
    rel->pathlist.push_back(seqpath);

    // TODO: create index paths when indexes exist on the relation.
    // For ClickBench, most queries use full table scans, so index path
    // generation is deferred until needed.

    // Select the cheapest path.
    Path* cheapest = nullptr;
    for (Path* path : rel->pathlist) {
        if (cheapest == nullptr || path->total_cost < cheapest->total_cost) {
            cheapest = path;
        }
    }
    rel->cheapest_path = cheapest;
    rel->rows = (cheapest != nullptr) ? cheapest->rows : 1.0;
}

// Build RelOptInfo for all base relations in the query's range table.
void BuildBaseRelInfos(PlannerInfo* root) {
    int rtindex = 1;
    for (Node* node : root->parse->rtable) {
        if (node == nullptr)
            continue;
        if (node->GetTag() != mytoydb::nodes::NodeTag::kRangeTblEntry) {
            root->simple_rel_array.push_back(nullptr);
            rtindex++;
            continue;
        }
        auto* rte = static_cast<RangeTblEntry*>(node);
        if (rte->rtekind != RTEKind::kRelation) {
            root->simple_rel_array.push_back(nullptr);
            rtindex++;
            continue;
        }

        auto* rel = makePallocNode<RelOptInfo>();
        rel->relid = rte->relid;
        rel->relindex = rtindex;
        rel->rte = rte;
        root->simple_rel_array.push_back(rel);

        SetBaseRelPathlist(root, rel);
        rtindex++;
    }
}

}  // namespace mytoydb::optimizer
