// partitioning.hpp — umbrella header for the M9 partitioning module.
//
// Includes all three sub-headers so that downstream code can pull the whole
// module with a single #include, mirroring the layout of other MyToyDB
// modules (e.g. types, catalog).

#pragma once

#include "mytoydb/partitioning/partbounds.hpp"
#include "mytoydb/partitioning/partdesc.hpp"
#include "mytoydb/partitioning/partprune.hpp"
