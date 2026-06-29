// set_refs.h — Plan-reference finalization.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/setrefs.c.
//
// Finalizes a Plan tree after Path→Plan translation: fixes Var references so
// they point to the correct range table entries (scan nodes) or child plan
// outputs (upper nodes). For pgcpp's single-table workload, most of this is
// a no-op because the parser already sets Var.varno correctly and rtoffset=0.
#pragma once

#include "executor/plannodes.hpp"
#include "optimizer/planner.hpp"
#include "parser/primnodes.hpp"

namespace pgcpp::optimizer {

// set_plan_references — top-level entry. Walks the plan tree and fixes Var
// references. For single-table plans with rtoffset=0, this is largely a no-op
// but ensures the plan is structurally sound.
void set_plan_references(PlannerInfo* root, pgcpp::executor::Plan* plan);

}  // namespace pgcpp::optimizer
