// plancache.hpp — plan cache for prepared statements.
//
// Converted from PostgreSQL 15's src/include/utils/plancache.h and
// src/backend/utils/cache/plancache.c.
//
// The plan cache stores execution plans for prepared statements so that
// repeated EXECUTE calls don't re-plan the query. Plans are invalidated
// when DDL changes the catalog (tracked via the catalog generation counter
// in inval.hpp).
//
// Architecture:
//   CachedPlanSource — holds the analyzed Query tree, can build a CachedPlan
//   CachedPlan       — holds the executor Plan tree + validity info
//
// Usage:
//   auto* source = new CachedPlanSource(query, query_string);
//   CachedPlan* plan = source->GetCachedPlan();  // builds or reuses
//   // ... execute plan ...
//   // Later, after DDL:
//   plan = source->GetCachedPlan();  // re-plans if invalidated

#pragma once

#include <cstdint>
#include <string>

#include "executor/plannodes.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::utils {

// CachedPlan — a cached execution plan.
//
// Stores the Plan tree and the catalog generation at creation time.
// A plan is valid as long as the catalog generation hasn't changed.
class CachedPlan {
public:
    // Construct a cached plan from an executor Plan tree.
    // Records the current catalog generation.
    explicit CachedPlan(pgcpp::executor::Plan* plan);

    // The executor Plan tree (owned by the plan cache).
    pgcpp::executor::Plan* plan() const { return plan_; }

    // The catalog generation when this plan was built.
    uint64_t generation() const { return generation_; }

    // True if the plan is still valid (catalog hasn't changed).
    bool IsValid() const;

private:
    pgcpp::executor::Plan* plan_;
    uint64_t generation_;
};

// CachedPlanSource — source for generating cached plans.
//
// Holds the analyzed Query tree. On first call to GetCachedPlan(), it
// plans the query and caches the result. On subsequent calls, it returns
// the cached plan if still valid, or re-plans if invalidated.
class CachedPlanSource {
public:
    // Construct a plan source from an analyzed Query tree.
    // The Query tree is NOT owned by the plan source (it's owned by the
    // PreparedStatement).
    CachedPlanSource(pgcpp::parser::Query* query, const std::string& query_string);

    // Destructor — frees the CachedPlan wrapper (the Plan tree inside is
    // palloc'd and owned by the memory context).
    ~CachedPlanSource();

    // Get a cached plan for this query. If the cached plan is still valid,
    // returns it. If invalidated (or first call), builds a new plan.
    // Returns nullptr if planning fails.
    CachedPlan* GetCachedPlan();

    // True if the cached plan (if any) is still valid.
    bool HasValidCachedPlan() const;

    // The Query tree this source plans.
    pgcpp::parser::Query* query() const { return query_; }

    // The original query string (for debugging/explain).
    const std::string& query_string() const { return query_string_; }

private:
    pgcpp::parser::Query* query_;
    std::string query_string_;
    CachedPlan* cached_plan_ = nullptr;
};

}  // namespace pgcpp::utils
