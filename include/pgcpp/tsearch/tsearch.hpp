#pragma once

// ---------------------------------------------------------------------------
// tsearch.hpp — umbrella header for the full-text search module (M15.20.6).
//
// Includes the public headers for the entire tsearch subsystem. Use this for
// convenience; otherwise include only the specific headers you need.
// ---------------------------------------------------------------------------

#include "mytoydb/tsearch/dict.hpp"
#include "mytoydb/tsearch/ispell.hpp"
#include "mytoydb/tsearch/thesaurus.hpp"
#include "mytoydb/tsearch/to_tsquery.hpp"
#include "mytoydb/tsearch/to_tsvector.hpp"
#include "mytoydb/tsearch/ts_typanalyze.hpp"
#include "mytoydb/tsearch/ts_utils.hpp"
#include "mytoydb/tsearch/tsquery_parser.hpp"
#include "mytoydb/tsearch/tsvector_parser.hpp"
#include "mytoydb/tsearch/wparser.hpp"
