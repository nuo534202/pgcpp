// partitioning.hpp — umbrella header for the M9 partitioning module.
//
// Includes all three sub-headers so that downstream code can pull the whole
// module with a single #include, mirroring the layout of other pgcpp
// modules (e.g. types, catalog).

#pragma once

#include "partitioning/partbounds.hpp"
#include "partitioning/partdesc.hpp"
#include "partitioning/partprune.hpp"
