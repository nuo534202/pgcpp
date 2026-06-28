// xlogutils.h — WAL utility functions (LSN comparison, etc.).
//
// Converted from PostgreSQL 15's src/backend/access/transam/xlogutils.c.
//
// LSN (XLogRecPtr) comparison helpers matching PostgreSQL's XLByte* macros.
#pragma once

#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace mytoydb::transaction {

// XLByteLT — lsn1 < lsn2
inline bool XLByteLT(XLogRecPtr lsn1, XLogRecPtr lsn2) {
    return lsn1 < lsn2;
}

// XLByteLE — lsn1 <= lsn2
inline bool XLByteLE(XLogRecPtr lsn1, XLogRecPtr lsn2) {
    return lsn1 <= lsn2;
}

// XLByteEQ — lsn1 == lsn2
inline bool XLByteEQ(XLogRecPtr lsn1, XLogRecPtr lsn2) {
    return lsn1 == lsn2;
}

// XLByteGT — lsn1 > lsn2
inline bool XLByteGT(XLogRecPtr lsn1, XLogRecPtr lsn2) {
    return lsn1 > lsn2;
}

// XLByteGE — lsn1 >= lsn2
inline bool XLByteGE(XLogRecPtr lsn1, XLogRecPtr lsn2) {
    return lsn1 >= lsn2;
}

}  // namespace mytoydb::transaction
