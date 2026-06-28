// partitioning.hpp — umbrella header for the M9 partitioning module.
//
// Includes all three sub-headers so that downstream code can pull the whole
// module with a single #include, mirroring the layout of other MyToyDB
// modules (e.g. types, catalog).

#pragma once

#include "pgcpp/partitioning/partbounds.hpp"
#include "pgcpp/partitioning/partdesc.hpp"
#include "pgcpp/partitioning/partprune.hpp"
