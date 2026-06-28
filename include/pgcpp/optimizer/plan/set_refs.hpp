// set_refs.h — Plan-reference finalization.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/setrefs.c.
//
// Finalizes a Plan tree after Path→Plan translation: fixes Var references so
// they point to the correct range table entries (scan nodes) or child plan
// outputs (upper nodes). For MyToyDB's single-table workload, most of this is
// a no-op because the parser already sets Var.varno correctly and rtoffset=0.
#pragma once

#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace mytoydb::optimizer {

// set_plan_references — top-level entry. Walks the plan tree and fixes Var
// references. For single-table plans with rtoffset=0, this is largely a no-op
// but ensures the plan is structurally sound.
void set_plan_references(PlannerInfo* root, mytoydb::executor::Plan* plan);

}  // namespace mytoydb::optimizer
