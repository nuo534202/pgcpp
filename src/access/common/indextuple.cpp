// indextuple.cpp — Index tuple construction and deformation.
//
// Converted from PostgreSQL 15's src/backend/access/common/indextuple.c.
//
// Provides index_form_tuple (build an IndexTuple from Datums + null array)
// and index_deform_tuple (extract Datums from an IndexTuple). The key-data
// layout follows the same alignment rules as heap tuples, starting after the
// 8-byte IndexTupleData header and optional null bitmap.

#include "pgcpp/access/indextuple.hpp"

#include <cstring>

#include "pgcpp/access/heapam.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::access {
using pgcpp::memory::palloc;
using pgcpp::transaction::ItemPointerData;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::VARSIZE;

uint32_t index_compute_data_size(TupleDesc tupdesc, const Datum* values, const bool* isnull) {
    uint32_t data_size = 0;
    for (int i = 0; i < tupdesc->natts; i++) {
        if (isnull != nullptr && isnull[i])
            continue;
        const auto& attr = tupdesc->attrs[i];
        data_size = att_align(data_size, attr.attalign);
        if (attr.attlen == -1) {
            // varlena: 4-byte header + data
            data_size += static_cast<uint32_t>(VARSIZE(DatumGetTextP(values[i])));
        } else {
            data_size += static_cast<uint32_t>(attr.attlen);
        }
    }
    return data_size;
}

IndexTuple index_form_tuple(TupleDesc tupdesc, const Datum* values, const bool* isnull,
                            const ItemPointerData& tid) {
    // Check for nulls.
    bool hasnull = false;
    for (int i = 0; i < tupdesc->natts; i++) {
        if (isnull != nullptr && isnull[i]) {
            hasnull = true;
            break;
        }
    }

    uint32_t data_size = index_compute_data_size(tupdesc, values, isnull);

    // Compute header size: fixed header + null bitmap (if any).
    uint32_t header_size = static_cast<uint32_t>(kIndexTupleHeaderSize);
    if (hasnull) {
        int null_bitmap_size = (tupdesc->natts + 7) / 8;
        header_size += static_cast<uint32_t>(null_bitmap_size);
    }
    // The index header is already 8-byte aligned; the bitmap may push past 8,
    // so we MAXALIGN the combined header+bitmap before the data.
    uint32_t hoff = att_align_max(header_size);

    uint32_t tuple_size = hoff + data_size;
    if (tuple_size > kIndexSizeMask) {
        ereport(pgcpp::error::LogLevel::kError, "index_form_tuple: index tuple size " +
                                                    std::to_string(tuple_size) + " exceeds max " +
                                                    std::to_string(kIndexSizeMask));
    }

    // Allocate the tuple data (header + user data).
    char* data = static_cast<char*>(palloc(tuple_size));
    std::memset(data, 0, hoff);
    auto* header = reinterpret_cast<IndexTupleData*>(data);

    // Set header fields.
    header->t_tid = tid;
    header->t_info = static_cast<uint16_t>(tuple_size);
    if (hasnull) {
        header->t_info |= kIndexNullMask;
    }

    // Fill null bitmap.
    if (hasnull) {
        uint8_t* null_bitmap = reinterpret_cast<uint8_t*>(data + kIndexTupleHeaderSize);
        for (int i = 0; i < tupdesc->natts; i++) {
            if (isnull != nullptr && isnull[i]) {
                null_bitmap[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
            }
        }
    }

    // Copy data values.
    uint32_t offset = hoff;
    bool hasvarwidth = false;
    for (int i = 0; i < tupdesc->natts; i++) {
        if (isnull != nullptr && isnull[i])
            continue;
        const auto& attr = tupdesc->attrs[i];
        offset = att_align(offset, attr.attalign);

        if (attr.attlen == -1) {
            // varlena: copy the full varlena (header + data).
            const char* src = DatumGetTextP(values[i]);
            int len = VARSIZE(src);
            std::memcpy(data + offset, src, len);
            offset += len;
            hasvarwidth = true;
        } else if (attr.attbyval) {
            // by-value: copy attlen bytes from the Datum.
            // On little-endian, the low bytes hold the value.
            std::memcpy(data + offset, &values[i], attr.attlen);
            offset += attr.attlen;
        } else {
            // by-reference, fixed length: copy from the pointer.
            std::memcpy(data + offset, reinterpret_cast<void*>(values[i]), attr.attlen);
            offset += attr.attlen;
            hasvarwidth = true;
        }
    }

    if (hasvarwidth) {
        header->t_info |= kIndexVarMask;
    }

    return header;
}

void index_deform_tuple(IndexTuple tup, TupleDesc tupdesc, Datum* values, bool* isnull) {
    char* data = reinterpret_cast<char*>(tup);
    uint16_t tuple_size = IndexTupleSize(tup);
    (void)tuple_size;

    bool hasnull = IndexTupleHasNulls(tup);
    uint8_t* null_bitmap = nullptr;
    if (hasnull) {
        null_bitmap = reinterpret_cast<uint8_t*>(data + kIndexTupleHeaderSize);
    }

    // The data starts after the MAXALIGN'd (header + bitmap).
    uint32_t header_size = static_cast<uint32_t>(kIndexTupleHeaderSize);
    if (hasnull) {
        header_size += static_cast<uint32_t>((tupdesc->natts + 7) / 8);
    }
    uint32_t hoff = att_align_max(header_size);

    uint32_t offset = hoff;
    for (int i = 0; i < tupdesc->natts; i++) {
        if (isnull != nullptr)
            isnull[i] = false;

        if (hasnull && (null_bitmap[i / 8] & (1 << (i % 8)))) {
            values[i] = 0;
            if (isnull != nullptr)
                isnull[i] = true;
            continue;
        }

        const auto& attr = tupdesc->attrs[i];
        offset = att_align(offset, attr.attalign);

        if (attr.attlen == -1) {
            // varlena: Datum points into the tuple.
            values[i] = Datum(data + offset);
            offset += static_cast<uint32_t>(VARSIZE(data + offset));
        } else if (attr.attbyval) {
            // by-value: copy attlen bytes into the Datum.
            Datum d = 0;
            std::memcpy(&d, data + offset, attr.attlen);
            values[i] = d;
            offset += attr.attlen;
        } else {
            // by-reference, fixed length: Datum points into the tuple.
            values[i] = Datum(data + offset);
            offset += attr.attlen;
        }
    }
}

IndexTuple CopyIndexTuple(IndexTuple source) {
    uint16_t size = IndexTupleSize(source);
    char* data = static_cast<char*>(palloc(size));
    std::memcpy(data, source, size);
    return reinterpret_cast<IndexTuple>(data);
}

}  // namespace pgcpp::access
