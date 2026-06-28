#pragma once

#include <cstdint>
#include <string>

#include "mytoydb/catalog/catalog.hpp"

namespace mytoydb::catalog {

// FormData_pg_aggregate — C++ equivalent of PostgreSQL's catalog/pg_aggregate.h.
//
// Each row extends a pg_proc entry (identified by aggfnoid) with aggregate-
// specific information: transition function, final function, etc.

// aggkind values (PostgreSQL 'char' codes).
enum class AggKind : char {
    kNormal = 'n',        // normal aggregate
    kOrderedSet = 'o',    // ordered-set aggregate
    kHypothetical = 'h',  // hypothetical-set aggregate
};

// aggfinalmodify / aggmfinalmodify values.
enum class AggModify : char {
    kReadOnly = 'r',   // finalfn does not modify transition state
    kShareable = 's',  // finalfn modifies state, but it can be shared
    kReadWrite = 'w',  // finalfn modifies state, cannot be shared
};

inline bool AggKindIsOrderedSet(AggKind kind) {
    return kind != AggKind::kNormal;
}

struct FormData_pg_aggregate {
    Oid aggfnoid = kInvalidOid;  // pg_proc OID of the aggregate
    AggKind aggkind = AggKind::kNormal;
    int16_t aggnumdirectargs = 0;      // number of "direct" arguments
    Oid aggtransfn = kInvalidOid;      // transition function (regproc)
    Oid aggfinalfn = kInvalidOid;      // final function (0 if none)
    Oid aggcombinefn = kInvalidOid;    // combine function (0 if none)
    Oid aggserialfn = kInvalidOid;     // transtype-to-bytea function
    Oid aggdeserialfn = kInvalidOid;   // bytea-to-transtype function
    Oid aggmtransfn = kInvalidOid;     // forward function for moving-agg
    Oid aggminvtransfn = kInvalidOid;  // inverse function for moving-agg
    Oid aggmfinalfn = kInvalidOid;     // final function for moving-agg
    bool aggfinalextra = false;        // pass extra dummy args to aggfinalfn
    bool aggmfinalextra = false;       // pass extra dummy args to aggmfinalfn
    AggModify aggfinalmodify = AggModify::kReadOnly;
    AggModify aggmfinalmodify = AggModify::kReadOnly;
    Oid aggsortop = kInvalidOid;      // associated sort operator
    Oid aggtranstype = kInvalidOid;   // type of transition (state) data
    int32_t aggtransspace = 0;        // estimated size of state data
    Oid aggmtranstype = kInvalidOid;  // type of moving-agg state data
    int32_t aggmtransspace = 0;       // estimated size of moving-agg state
    std::string agginitval;           // initial value for transition state
    std::string aggminitval;          // initial value for moving-agg state
};

using Form_pg_aggregate = FormData_pg_aggregate*;

}  // namespace mytoydb::catalog
