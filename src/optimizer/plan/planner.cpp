// planner.cpp — Top-level planner entry point.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/planner.c.
//
// The planner takes a parser Query tree and produces an executor Plan tree.
// For SELECT, it delegates to subplanner(). For INSERT/UPDATE/DELETE, it
// plans the source query and wraps the result in a ModifyTable node.
#include "mytoydb/optimizer/planner.hpp"

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::optimizer {
using mytoydb::nodes::makePallocNode;

using mytoydb::executor::ModifyTable;
using mytoydb::executor::Plan;
using mytoydb::parser::CmdType;
using mytoydb::parser::Node;
using mytoydb::parser::Query;
using mytoydb::parser::TargetEntry;

// Forward declaration — implemented in subplanner.cpp.
Plan* subplanner(PlannerInfo* root);

Plan* planner(Query* query) {
    if (query == nullptr)
        return nullptr;

    // Create PlannerInfo.
    auto* root = makePallocNode<PlannerInfo>();
    root->parse = query;

    // Handle LIMIT.
    if (query->limit_count != nullptr) {
        // Try to extract a constant limit value.
        // For now, just set a flag; the actual value is handled in subplanner.
        root->limit_tuples = -1;  // Will be set from the Const if possible.
    }

    Plan* plan = nullptr;

    switch (query->command_type) {
        case CmdType::kSelect:
            plan = subplanner(root);
            break;

        case CmdType::kInsert:
        case CmdType::kUpdate:
        case CmdType::kDelete: {
            // Plan the source query (the SELECT part of INSERT ... SELECT,
            // or the scan for UPDATE/DELETE).
            plan = subplanner(root);

            // Wrap in ModifyTable.
            auto* mt = makePallocNode<ModifyTable>();
            mt->operation = query->command_type;
            mt->resultRelid = query->result_relation;
            mt->lefttree = plan;
            // Copy target list for RETURNING (if any).
            for (Node* te : query->returning_list) {
                mt->targetlist.push_back(static_cast<mytoydb::parser::TargetEntry*>(te));
            }
            plan = mt;
            break;
        }

        default:
            // Other command types (UTILITY, MERGE) are not yet supported.
            break;
    }

    return plan;
}

}  // namespace mytoydb::optimizer
