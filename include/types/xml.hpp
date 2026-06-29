#pragma once

#include <cstdint>
#include <string>

#include "types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// XML type (PostgreSQL utils/adt/xml.c).
//
// We expose a simplified text-stored XML representation that preserves the
// input string verbatim and provides:
//   - xml_in/xml_out          (round-trip; minimal sanity check)
//   - xml_validate            (well-formedness check; not schema-aware)
//   - xml_concat              (concatenation)
//   - xpath_exists            (literal substring check — simplified)
//
// Storage: a varlena text identical to the `text` type. Datum is a pointer.
// ---------------------------------------------------------------------------

Datum xml_in(const char* str);
char* xml_out(Datum value);

// Returns true if the XML string is "well-formed" — i.e. each '<' has a
// matching '>', tags nest properly, and there is exactly one root element.
Datum xml_validate(Datum value);

Datum xml_concat(Datum a, Datum b);
Datum xpath_exists(Datum xml, Datum xpath);

}  // namespace pgcpp::types
