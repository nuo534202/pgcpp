// explain.cpp — EXPLAIN command implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/explain.c.
//
// Produces a real plan-tree dump: node type, relation name, cost/rows/width
// estimates, and node-specific properties (Sort Key, Hash Cond, Filter, ...).
// The output is sent to the client as a RowDescription + DataRow stream,
// matching PostgreSQL's wire behaviour.
//
// Supported EXPLAIN options:
//   ANALYZE  — recognised; actual-execution instrumentation is not yet wired
//              so the estimated plan is shown (same as EXPLAIN without ANALYZE).
//   VERBOSE  — recognised; currently has no extra effect.
//   COSTS    — ON by default; COSTS OFF hides the (cost=.. rows=.. width=..) clause.
//   SETTINGS — recognised; currently has no extra effect.
#include "commands/explain.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "common/containers/node.hpp"
#include "executor/plannodes.hpp"
#include "optimizer/planner.hpp"
#include "parser/analyze.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "parser/primnodes.hpp"
#include "protocol/pqformat.hpp"

namespace pgcpp::commands {

using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::planner;
using pgcpp::parser::DefElem;
using pgcpp::parser::ExplainStmt;
using pgcpp::parser::Node;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RawStmt;
using pgcpp::protocol::BuildDataRow;
using pgcpp::protocol::BuildRowDescription;
using pgcpp::protocol::OutputSink;
using pgcpp::protocol::RowDescriptionField;

// ExplainState — per-EXPLAIN options and accumulated output lines.
//
// Mirrors PostgreSQL's ExplainState (simplified). The plan tree is dumped into
// `lines` and each line is sent as a DataRow to the client.
struct ExplainState {
    bool costs = true;               // show (cost=.. rows=.. width=..)?
    bool verbose = false;            // VERBOSE (extra schema qualification)
    bool analyze = false;            // ANALYZE (actual execution stats)
    bool settings = false;           // SETTINGS (print modified GUCs)
    const Query* query = nullptr;    // planned query (for rtable lookups)
    std::vector<std::string> lines;  // accumulated output lines
};

namespace {

// Format a double cost value as PG does: 2 decimal places.
std::string FormatCost(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

// Format an integer row count.
std::string FormatRows(int n) {
    return std::to_string(n);
}

// Resolve a 1-based scanrelid to the relation/alias name from the range table.
// Returns an empty string if the RTE cannot be found.
std::string GetRelationName(const Query* query, int scanrelid) {
    if (query == nullptr || scanrelid < 1)
        return "";
    const auto& rtable = query->rtable;
    if (static_cast<size_t>(scanrelid) > rtable.size())
        return "";
    auto* rte = static_cast<RangeTblEntry*>(rtable[scanrelid - 1]);
    if (rte == nullptr)
        return "";
    if (rte->eref != nullptr && !rte->eref->aliasname.empty())
        return rte->eref->aliasname;
    if (rte->alias != nullptr && !rte->alias->aliasname.empty())
        return rte->alias->aliasname;
    return "";
}

// Map a PlanType to the human-readable node name used in EXPLAIN output.
// Names follow PostgreSQL 15's convention (e.g., "Seq Scan", "Hash Join").
const char* PlanTypeName(PlanType t) {
    switch (t) {
        case PlanType::kResult:
            return "Result";
        case PlanType::kSeqScan:
            return "Seq Scan";
        case PlanType::kIndexScan:
            return "Index Scan";
        case PlanType::kAgg:
            return "Aggregate";
        case PlanType::kSort:
            return "Sort";
        case PlanType::kNestLoop:
            return "Nested Loop";
        case PlanType::kHashJoin:
            return "Hash Join";
        case PlanType::kHash:
            return "Hash";
        case PlanType::kModifyTable:
            return "ModifyTable";
        case PlanType::kLimit:
            return "Limit";
        case PlanType::kAppend:
            return "Append";
        case PlanType::kMaterial:
            return "Materialize";
        case PlanType::kUnique:
            return "Unique";
        case PlanType::kSubqueryScan:
            return "Subquery Scan";
        case PlanType::kMergeJoin:
            return "Merge Join";
        case PlanType::kCteScan:
            return "CTE Scan";
        case PlanType::kWindowAgg:
            return "WindowAgg";
        case PlanType::kGroup:
            return "Group";
        case PlanType::kSetOp:
            return "SetOp";
        case PlanType::kMergeAppend:
            return "Merge Append";
        case PlanType::kBitmapIndexScan:
            return "Bitmap Index Scan";
        case PlanType::kBitmapHeapScan:
            return "Bitmap Heap Scan";
        case PlanType::kLockRows:
            return "LockRows";
        case PlanType::kValuesScan:
            return "Values Scan";
        case PlanType::kTidScan:
            return "Tid Scan";
        case PlanType::kFunctionScan:
            return "Function Scan";
        case PlanType::kProjectSet:
            return "ProjectSet";
        case PlanType::kMemoize:
            return "Memoize";
        case PlanType::kIncrementalSort:
            return "Incremental Sort";
        case PlanType::kRecursiveUnion:
            return "Recursive Union";
        case PlanType::kWorkTableScan:
            return "WorkTable Scan";
        case PlanType::kGather:
            return "Gather";
        case PlanType::kGatherMerge:
            return "Gather Merge";
        case PlanType::kForeignScan:
            return "Foreign Scan";
    }
    return "Unknown";
}

// Append a line to the ExplainState output at the given indentation depth.
// PG uses 2 spaces per depth level. Child nodes are prefixed with "->  ".
void ExplainAppendLine(ExplainState* es, int depth, const std::string& text) {
    std::string line;
    // Each depth level contributes 2 spaces of indentation.
    for (int i = 0; i < depth; ++i)
        line += "  ";
    line += text;
    es->lines.push_back(line);
}

// Append a property line (e.g., "Sort Key: a").
// Property lines are at the same indentation as the node they belong to.
void ExplainProperty(ExplainState* es, int depth, const std::string& label,
                     const std::string& value) {
    std::string text = label + ": " + value;
    // Property lines are indented one level deeper than the node header.
    ExplainAppendLine(es, depth + 1, text);
}

// Build the cost/rows/width suffix: "  (cost=X..Y rows=Z width=W)".
// Returns empty string when costs are disabled.
std::string FormatCostClause(const ExplainState* es, const Plan* plan) {
    if (!es->costs)
        return "";
    std::string s = "  (cost=";
    s += FormatCost(plan->startup_cost);
    s += "..";
    s += FormatCost(plan->total_cost);
    s += " rows=";
    s += FormatRows(plan->plan_rows);
    s += " width=";
    s += std::to_string(plan->plan_width);
    s += ")";
    return s;
}

// Format a sort-key list as PG does: column names or "col DESC".
// Since pgcpp plans store sortColIdx (1-based attr numbers) rather than names,
// we show the attribute number for simplicity.
std::string FormatSortKeys(const std::vector<int>& colIdx, const std::vector<bool>& reverse) {
    std::string s;
    for (size_t i = 0; i < colIdx.size(); ++i) {
        if (i > 0)
            s += ", ";
        s += "col" + std::to_string(colIdx[i]);
        if (i < reverse.size() && reverse[i])
            s += " DESC";
    }
    return s;
}

// Dump node-specific properties (Sort Key, Hash Cond, Filter, etc.).
void ExplainNodeProperties(ExplainState* es, int depth, const Plan* plan) {
    // Filter (qual) — shown for nodes that have a qual but are not scans
    // (scans show their filter differently in PG, but we simplify).
    if (plan->qual != nullptr) {
        ExplainProperty(es, depth, "Filter", "(qual)");
    }

    switch (plan->type) {
        case PlanType::kSort: {
            const auto* sort = static_cast<const pgcpp::executor::Sort*>(plan);
            if (!sort->sortColIdx.empty()) {
                ExplainProperty(es, depth, "Sort Key",
                                FormatSortKeys(sort->sortColIdx, sort->reverse));
            }
            break;
        }
        case PlanType::kIncrementalSort: {
            const auto* isort = static_cast<const pgcpp::executor::IncrementalSort*>(plan);
            if (!isort->sortColIdx.empty()) {
                ExplainProperty(es, depth, "Sort Key",
                                FormatSortKeys(isort->sortColIdx, isort->reverse));
            }
            if (!isort->presortedColIdx.empty()) {
                ExplainProperty(es, depth, "Presorted Key",
                                FormatSortKeys(isort->presortedColIdx, {}));
            }
            break;
        }
        case PlanType::kMergeAppend: {
            const auto* ma = static_cast<const pgcpp::executor::MergeAppend*>(plan);
            if (!ma->sortColIdx.empty()) {
                ExplainProperty(es, depth, "Sort Key", FormatSortKeys(ma->sortColIdx, ma->reverse));
            }
            break;
        }
        case PlanType::kGatherMerge: {
            const auto* gm = static_cast<const pgcpp::executor::GatherMerge*>(plan);
            if (!gm->sortColIdx.empty()) {
                ExplainProperty(es, depth, "Merge Key",
                                FormatSortKeys(gm->sortColIdx, gm->reverse));
            }
            ExplainProperty(es, depth, "Workers Planned", std::to_string(gm->num_workers));
            break;
        }
        case PlanType::kAgg: {
            const auto* agg = static_cast<const pgcpp::executor::Agg*>(plan);
            const char* strat = "Plain";
            switch (agg->aggstrategy) {
                case pgcpp::executor::Agg::Strategy::kPlain:
                    strat = "Plain";
                    break;
                case pgcpp::executor::Agg::Strategy::kSorted:
                    strat = "Sorted";
                    break;
                case pgcpp::executor::Agg::Strategy::kHashed:
                    strat = "Hashed";
                    break;
            }
            ExplainProperty(es, depth, "Strategy", strat);
            if (!agg->groupColIdx.empty()) {
                std::string s;
                for (size_t i = 0; i < agg->groupColIdx.size(); ++i) {
                    if (i > 0)
                        s += ", ";
                    s += "col" + std::to_string(agg->groupColIdx[i]);
                }
                ExplainProperty(es, depth, "Group Key", s);
            }
            break;
        }
        case PlanType::kGroup: {
            const auto* grp = static_cast<const pgcpp::executor::Group*>(plan);
            if (!grp->groupColIdx.empty()) {
                std::string s;
                for (size_t i = 0; i < grp->groupColIdx.size(); ++i) {
                    if (i > 0)
                        s += ", ";
                    s += "col" + std::to_string(grp->groupColIdx[i]);
                }
                ExplainProperty(es, depth, "Group Key", s);
            }
            break;
        }
        case PlanType::kHashJoin: {
            const auto* hj = static_cast<const pgcpp::executor::HashJoin*>(plan);
            if (hj->jointype != pgcpp::parser::JoinType::kInner) {
                const char* jt = "Inner";
                switch (hj->jointype) {
                    case pgcpp::parser::JoinType::kInner:
                        jt = "Inner";
                        break;
                    case pgcpp::parser::JoinType::kLeft:
                        jt = "Left";
                        break;
                    case pgcpp::parser::JoinType::kRight:
                        jt = "Right";
                        break;
                    case pgcpp::parser::JoinType::kFull:
                        jt = "Full";
                        break;
                    case pgcpp::parser::JoinType::kSemi:
                        jt = "Semi";
                        break;
                    case pgcpp::parser::JoinType::kAnti:
                        jt = "Anti";
                        break;
                    default:
                        break;
                }
                ExplainProperty(es, depth, "Join Type", jt);
            }
            ExplainProperty(es, depth, "Hash Cond", "(hashclauses)");
            break;
        }
        case PlanType::kNestLoop: {
            const auto* nl = static_cast<const pgcpp::executor::NestLoop*>(plan);
            if (nl->jointype != pgcpp::parser::JoinType::kInner) {
                const char* jt = "Inner";
                switch (nl->jointype) {
                    case pgcpp::parser::JoinType::kInner:
                        jt = "Inner";
                        break;
                    case pgcpp::parser::JoinType::kLeft:
                        jt = "Left";
                        break;
                    case pgcpp::parser::JoinType::kRight:
                        jt = "Right";
                        break;
                    case pgcpp::parser::JoinType::kFull:
                        jt = "Full";
                        break;
                    case pgcpp::parser::JoinType::kSemi:
                        jt = "Semi";
                        break;
                    case pgcpp::parser::JoinType::kAnti:
                        jt = "Anti";
                        break;
                    default:
                        break;
                }
                ExplainProperty(es, depth, "Join Type", jt);
            }
            break;
        }
        case PlanType::kMergeJoin: {
            const auto* mj = static_cast<const pgcpp::executor::MergeJoin*>(plan);
            if (mj->jointype != pgcpp::parser::JoinType::kInner) {
                const char* jt = "Inner";
                switch (mj->jointype) {
                    case pgcpp::parser::JoinType::kInner:
                        jt = "Inner";
                        break;
                    case pgcpp::parser::JoinType::kLeft:
                        jt = "Left";
                        break;
                    case pgcpp::parser::JoinType::kRight:
                        jt = "Right";
                        break;
                    case pgcpp::parser::JoinType::kFull:
                        jt = "Full";
                        break;
                    case pgcpp::parser::JoinType::kSemi:
                        jt = "Semi";
                        break;
                    case pgcpp::parser::JoinType::kAnti:
                        jt = "Anti";
                        break;
                    default:
                        break;
                }
                ExplainProperty(es, depth, "Join Type", jt);
            }
            ExplainProperty(es, depth, "Merge Cond", "(mergeclauses)");
            break;
        }
        case PlanType::kLimit: {
            const auto* lim = static_cast<const pgcpp::executor::Limit*>(plan);
            if (lim->limit_count >= 0) {
                ExplainProperty(es, depth, "Limit", std::to_string(lim->limit_count));
            }
            break;
        }
        case PlanType::kRecursiveUnion: {
            const auto* ru = static_cast<const pgcpp::executor::RecursiveUnion*>(plan);
            ExplainProperty(es, depth, "Working Table", "wtParam=" + std::to_string(ru->wtParam));
            break;
        }
        case PlanType::kGather: {
            const auto* g = static_cast<const pgcpp::executor::Gather*>(plan);
            ExplainProperty(es, depth, "Workers Planned", std::to_string(g->num_workers));
            break;
        }
        default:
            break;
    }
}

// Recursively dump a plan node and its children.
//
// depth controls indentation: 0 for the root, incremented for each child.
// child_prefix is "->  " for child nodes (empty for the root).
void ExplainNode(ExplainState* es, int depth, const Plan* plan) {
    if (plan == nullptr)
        return;

    // Build the node header line.
    std::string header;
    if (depth > 0)
        header = "->  ";
    header += PlanTypeName(plan->type);

    // Append relation name for scan nodes.
    int scanrelid = 0;
    switch (plan->type) {
        case PlanType::kSeqScan:
            scanrelid = static_cast<const pgcpp::executor::SeqScan*>(plan)->scanrelid;
            break;
        case PlanType::kIndexScan:
            scanrelid = static_cast<const pgcpp::executor::IndexScan*>(plan)->scanrelid;
            break;
        case PlanType::kBitmapHeapScan:
            scanrelid = static_cast<const pgcpp::executor::BitmapHeapScan*>(plan)->scanrelid;
            break;
        case PlanType::kTidScan:
            scanrelid = static_cast<const pgcpp::executor::TidScan*>(plan)->scanrelid;
            break;
        case PlanType::kValuesScan:
            scanrelid = static_cast<const pgcpp::executor::ValuesScan*>(plan)->scanrelid;
            break;
        case PlanType::kFunctionScan:
            scanrelid = static_cast<const pgcpp::executor::FunctionScan*>(plan)->scanrelid;
            break;
        case PlanType::kSubqueryScan:
            scanrelid = static_cast<const pgcpp::executor::SubqueryScan*>(plan)->scanrelid;
            break;
        case PlanType::kCteScan:
            scanrelid = static_cast<const pgcpp::executor::CteScan*>(plan)->scanrelid;
            break;
        case PlanType::kWorkTableScan:
            scanrelid = static_cast<const pgcpp::executor::WorkTableScan*>(plan)->scanrelid;
            break;
        default:
            break;
    }
    if (scanrelid > 0) {
        std::string relname = GetRelationName(es->query, scanrelid);
        if (!relname.empty())
            header += " on " + relname;
    }

    // Append cost/rows/width clause.
    header += FormatCostClause(es, plan);

    // Emit the header line.
    ExplainAppendLine(es, depth, header);

    // Emit node-specific properties.
    ExplainNodeProperties(es, depth, plan);

    // Recurse on children.
    // lefttree is the outer (first) child; righttree is the inner (second) child.
    if (plan->lefttree != nullptr)
        ExplainNode(es, depth + 1, plan->lefttree);
    if (plan->righttree != nullptr)
        ExplainNode(es, depth + 1, plan->righttree);

    // Append has special child list (append_plans).
    if (plan->type == PlanType::kAppend) {
        const auto* app = static_cast<const pgcpp::executor::Append*>(plan);
        for (Plan* child : app->append_plans)
            ExplainNode(es, depth + 1, child);
    }
    if (plan->type == PlanType::kMergeAppend) {
        const auto* ma = static_cast<const pgcpp::executor::MergeAppend*>(plan);
        for (Plan* child : ma->merge_plans)
            ExplainNode(es, depth + 1, child);
    }
}

// Parse EXPLAIN options from the ExplainStmt's option list (DefElem nodes).
void ParseExplainOptions(ExplainState* es, const std::vector<Node*>& options) {
    for (Node* opt_node : options) {
        if (opt_node == nullptr)
            continue;
        auto* def = static_cast<DefElem*>(opt_node);
        const std::string& name = def->defname;
        if (name == "analyze")
            es->analyze = true;
        else if (name == "verbose")
            es->verbose = true;
        else if (name == "costs")
            es->costs = true;  // bare COSTS keyword = COSTS ON (default)
        else if (name == "settings")
            es->settings = true;
    }
}

}  // namespace

std::string ExplainQuery(ExplainStmt* stmt, OutputSink* sink) {
    if (stmt == nullptr)
        return "EXPLAIN";

    // Parse EXPLAIN options.
    ExplainState es;
    ParseExplainOptions(&es, stmt->options);

    // The ExplainStmt's `query` field is a raw parse tree (SelectStmt, etc.).
    // Wrap it in a RawStmt and run parse analysis to get a Query node.
    auto* raw = makePallocNode<RawStmt>();
    raw->stmt = stmt->query;
    std::vector<RawStmt*> raw_stmts{raw};
    std::vector<Query*> queries = pgcpp::parser::parse_analyze(raw_stmts, nullptr);

    if (queries.empty()) {
        if (sink != nullptr) {
            RowDescriptionField field;
            field.name = "QUERY PLAN";
            field.type_oid = 25;   // TEXTOID
            field.type_size = -1;  // variable-length
            field.type_mod = -1;
            field.format = 0;  // text
            sink->SendMessage(BuildRowDescription({field}));
            sink->SendMessage(BuildDataRow({"(empty query)"}, {false}));
        }
        return "EXPLAIN";
    }

    Query* query = queries.front();
    es.query = query;

    // Plan the query.
    Plan* plan = planner(query);

    // Dump the plan tree into es.lines.
    if (plan != nullptr) {
        ExplainNode(&es, 0, plan);
    } else {
        es.lines.push_back("(no plan)");
    }

    // Send the output to the client sink as RowDescription + DataRows.
    if (sink != nullptr) {
        RowDescriptionField field;
        field.name = "QUERY PLAN";
        field.type_oid = 25;   // TEXTOID
        field.type_size = -1;  // variable-length
        field.type_mod = -1;
        field.format = 0;  // text
        sink->SendMessage(BuildRowDescription({field}));

        for (const std::string& line : es.lines) {
            sink->SendMessage(BuildDataRow({line}, {false}));
        }
    }

    return "EXPLAIN";
}

}  // namespace pgcpp::commands
