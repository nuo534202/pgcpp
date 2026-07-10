// inval.hpp — catalog invalidation tracking for plan cache.
//
// Converted from PostgreSQL 15's src/backend/utils/cache/inval.c.
//
// PostgreSQL uses a sophisticated invalidation message system: DDL
// operations record invalidation messages that are processed at the next
// CommandCounterIncrement, selectively invalidating only the syscache and
// relcache entries that reference the changed objects.
//
// pgcpp uses a simplified model: a single global "catalog generation"
// counter. When any DDL modifies the catalog (CREATE/DROP/ALTER TABLE,
// CREATE/DROP INDEX, etc.), the counter is incremented. Cached plans
// record the generation at creation time; if the generation differs on
// next use, the plan is considered invalid and must be re-planned.
//
// This is conservative (plans may be invalidated even when the DDL doesn't
// affect them) but correct (plans are never used when stale).

#pragma once

#include <cstdint>

namespace pgcpp::utils {

// GetCatalogGeneration — return the current catalog generation number.
// Starts at 0 and increments on each DDL operation.
uint64_t GetCatalogGeneration();

// IncrementCatalogGeneration — bump the catalog generation counter.
// Called after any DDL that modifies the catalog (CREATE/DROP/ALTER TABLE,
// CREATE/DROP INDEX, CREATE/DROP SCHEMA, etc.).
// This invalidates all cached plans.
void IncrementCatalogGeneration();

// ResetCatalogGeneration — reset the counter to 0 (for testing only).
void ResetCatalogGeneration();

}  // namespace pgcpp::utils
