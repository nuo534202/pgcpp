#pragma once

// ---------------------------------------------------------------------------
// tsearch.hpp — umbrella header for the full-text search module (M15.20.6).
//
// Includes the public headers for the entire tsearch subsystem. Use this for
// convenience; otherwise include only the specific headers you need.
// ---------------------------------------------------------------------------

#include "pgcpp/tsearch/dict.hpp"
#include "pgcpp/tsearch/ispell.hpp"
#include "pgcpp/tsearch/thesaurus.hpp"
#include "pgcpp/tsearch/to_tsquery.hpp"
#include "pgcpp/tsearch/to_tsvector.hpp"
#include "pgcpp/tsearch/ts_typanalyze.hpp"
#include "pgcpp/tsearch/ts_utils.hpp"
#include "pgcpp/tsearch/tsquery_parser.hpp"
#include "pgcpp/tsearch/tsvector_parser.hpp"
#include "pgcpp/tsearch/wparser.hpp"
