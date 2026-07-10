// inval_test.cpp — Unit tests for catalog invalidation (M3/utils/cache).
//
// Tests the simplified catalog generation counter model used by the plan
// cache to track DDL invalidation.

#include "utils/cache/inval.hpp"

#include <gtest/gtest.h>

using pgcpp::utils::GetCatalogGeneration;
using pgcpp::utils::IncrementCatalogGeneration;
using pgcpp::utils::ResetCatalogGeneration;

TEST(InvalTest, InitialGenerationIsZero) {
    ResetCatalogGeneration();
    EXPECT_EQ(GetCatalogGeneration(), 0u);
}

TEST(InvalTest, IncrementAddsOne) {
    ResetCatalogGeneration();
    IncrementCatalogGeneration();
    EXPECT_EQ(GetCatalogGeneration(), 1u);
}

TEST(InvalTest, MultipleIncrements) {
    ResetCatalogGeneration();
    for (int i = 0; i < 10; ++i) {
        IncrementCatalogGeneration();
    }
    EXPECT_EQ(GetCatalogGeneration(), 10u);
}

TEST(InvalTest, ResetReturnsToZero) {
    IncrementCatalogGeneration();
    IncrementCatalogGeneration();
    IncrementCatalogGeneration();
    ResetCatalogGeneration();
    EXPECT_EQ(GetCatalogGeneration(), 0u);
}

TEST(InvalTest, IncrementAfterReset) {
    ResetCatalogGeneration();
    IncrementCatalogGeneration();
    IncrementCatalogGeneration();
    ResetCatalogGeneration();
    IncrementCatalogGeneration();
    EXPECT_EQ(GetCatalogGeneration(), 1u);
}
