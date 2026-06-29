#pragma once

// ---------------------------------------------------------------------------
// tsearch.hpp — umbrella header for the full-text search module (M15.20.6).
//
// Includes the public headers for the entire tsearch subsystem. Use this for
// convenience; otherwise include only the specific headers you need.
// ---------------------------------------------------------------------------

#include "tsearch/dict.hpp"
#include "tsearch/ispell.hpp"
#include "tsearch/thesaurus.hpp"
#include "tsearch/to_tsquery.hpp"
#include "tsearch/to_tsvector.hpp"
#include "tsearch/ts_typanalyze.hpp"
#include "tsearch/ts_utils.hpp"
#include "tsearch/tsquery_parser.hpp"
#include "tsearch/tsvector_parser.hpp"
#include "tsearch/wparser.hpp"
