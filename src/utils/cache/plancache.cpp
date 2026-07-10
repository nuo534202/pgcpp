// plancache.cpp — plan cache implementation.
//
// Converted from PostgreSQL 15's src/backend/utils/cache/plancache.c.
//
// The plan cache stores execution plans for prepared statements. On first
// use, the query is planned and the result is cached. On subsequent uses,
// the cached plan is returned if still valid (catalog generation hasn't
// changed). If invalidated by DDL, the query is re-planned.

#include "utils/cache/plancache.hpp"

#include "optimizer/planner.hpp"
#include "utils/cache/inval.hpp"

namespace pgcpp::utils {

// --- CachedPlan ---

CachedPlan::CachedPlan(pgcpp::executor::Plan* plan)
    : plan_(plan), generation_(GetCatalogGeneration()) {}

bool CachedPlan::IsValid() const {
    return generation_ == GetCatalogGeneration();
}

// --- CachedPlanSource ---

CachedPlanSource::CachedPlanSource(pgcpp::parser::Query* query, const std::string& query_string)
    : query_(query), query_string_(query_string) {}

CachedPlanSource::~CachedPlanSource() {
    delete cached_plan_;
}

CachedPlan* CachedPlanSource::GetCachedPlan() {
    // If we have a cached plan and it's still valid, return it.
    if (cached_plan_ != nullptr && cached_plan_->IsValid()) {
        return cached_plan_;
    }

    // The cached plan is stale (or doesn't exist). Re-plan.
    // Free the old cached plan if it exists.
    // Note: the Plan tree inside is palloc'd, so it lives in the memory
    // context and doesn't need explicit delete. The CachedPlan wrapper
    // itself is heap-allocated.
    delete cached_plan_;
    cached_plan_ = nullptr;

    // Plan the query.
    pgcpp::executor::Plan* plan = pgcpp::optimizer::planner(query_);
    if (plan == nullptr) {
        return nullptr;
    }

    // Create a new cached plan.
    cached_plan_ = new CachedPlan(plan);
    return cached_plan_;
}

bool CachedPlanSource::HasValidCachedPlan() const {
    return cached_plan_ != nullptr && cached_plan_->IsValid();
}

}  // namespace pgcpp::utils
