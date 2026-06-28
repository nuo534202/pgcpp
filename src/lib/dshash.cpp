// dshash.cpp — Dynamic shared hash table.
//
// DsHash is a template class with all implementations defined inline in the
// header (see mytoydb/lib/dshash.hpp). This translation unit exists so the
// file is part of the mytoydb_core build; it pulls in the header so the
// compiler validates template syntax even when no explicit instantiation is
// requested by callers.

#include "pgcpp/lib/dshash.hpp"

namespace mytoydb::lib {

// No out-of-line definitions: DsHash is header-only.

}  // namespace mytoydb::lib
