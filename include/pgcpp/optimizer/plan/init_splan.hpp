// init_splan.h — PlannerInfo / RelOptInfo initialization.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/initsplan.c.
//
// Initializes the planner's per-query state: builds the simple_rte_array and
// simple_rel_array, deconstructs the join tree (single-table case), and
// distributes WHERE-clause quals to the appropriate RelOptInfo baserestrictinfo.
#pragma once

#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/optimizer/util/restrictinfo.hpp"
#include "pgcpp/parser/parsenodes.hpp"

namespace pgcpp::optimizer {

// query_planner_init — initialize PlannerInfo for a query.
// Sets root->parse, builds simple_rte_array, builds base RelOptInfos, and
// distributes quals. This is the "initsplan.c" initialization portion of
// PG's query_planner (not the full query_planner, which lives in planner.cpp).
void query_planner_init(PlannerInfo* root, pgcpp::parser::Query* parse);

// build_base_rel_infos — build RelOptInfo for each base relation in the range
// table, populating simple_rel_array and filling catalog statistics. This is
// the PG-style replacement for allpaths.cpp's BuildBaseRelInfos (which is kept
// for the existing subplanner path).
void build_base_rel_infos(PlannerInfo* root);

// deconstruct_jointree — walk the query's join tree and add base relations.
// Simplified: for single-table queries, ensures the single base rel is present
// in simple_rel_array. Multi-table JOIN traversal is left as a TODO.
void deconstruct_jointree(PlannerInfo* root);

// add_base_rels_to_query — recursively add base relations from a jointree node.
// For RangeTblRef, calls build_simple_rel; for JoinExpr, recurses into larg/rarg.
void add_base_rels_to_query(PlannerInfo* root, pgcpp::parser::Node* jtnode);

// distribute_quals_to_rels — distribute WHERE-clause quals to the RelOptInfo
// of the relation(s) they reference. Single-table quals go to
// baserestrictinfo; multi-table quals go to joininfo (skeleton).
void distribute_quals_to_rels(PlannerInfo* root, pgcpp::parser::Node* quals);

}  // namespace pgcpp::optimizer
