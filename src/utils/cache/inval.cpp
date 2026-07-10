// inval.cpp — catalog invalidation tracking for plan cache.
//
// Converted from PostgreSQL 15's src/backend/utils/cache/inval.c.
//
// Simplified model: a single global counter that increments on each DDL.
// See inval.hpp for design rationale.

#include "utils/cache/inval.hpp"

namespace pgcpp::utils {

namespace {

// Global catalog generation counter. Starts at 0.
// Incremented by IncrementCatalogGeneration() (called after DDL).
uint64_t g_catalog_generation = 0;

}  // namespace

uint64_t GetCatalogGeneration() {
    return g_catalog_generation;
}

void IncrementCatalogGeneration() {
    g_catalog_generation++;
}

void ResetCatalogGeneration() {
    g_catalog_generation = 0;
}

}  // namespace pgcpp::utils
