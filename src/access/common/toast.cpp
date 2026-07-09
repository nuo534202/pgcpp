// toast.cpp — TOAST (The Oversized-Attribute Storage Technique) implementation.
//
// Converted from PostgreSQL 15's src/backend/access/common/toast_internals.c,
// toast_compression.c, and detoast.c.
//
// Implements:
//   - pglz compression/decompression (simplified LZ77)
//   - TOAST table creation and management
//   - toast_save_datum / toast_get_datum / toast_delete_datum
//   - toast_insert_or_update (auto-toast large values during INSERT)
//   - detoast_attr (auto-detoast on read)
//
// TOAST table schema (relkind = 't'):
//   chunk_id   oid    — identifies the toasted value
//   chunk_seq  int4   — sequence number (0, 1, 2, ...)
//   chunk_data bytea  — a chunk of the value (up to kToastMaxChunkSize bytes)

#include "access/toast.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "storage/bufmgr.hpp"
#include "storage/bufpage.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"
#include "types/varlena.hpp"

namespace pgcpp::access {

using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::memory::palloc;
using pgcpp::nodes::makePallocNode;
using pgcpp::transaction::CommandCounterIncrement;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::HeapTupleData;
using pgcpp::transaction::HeapTupleHeaderSetXmax;
using pgcpp::transaction::HeapTupleHeaderSetXmin;
using pgcpp::transaction::ItemPointerData;
using pgcpp::transaction::kHeapHasVarWidth;
using pgcpp::transaction::kHeapTupleHeaderSize;
using pgcpp::transaction::TransactionId;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::kVarAttCompressed;
using pgcpp::types::kVarAttExternal;
using pgcpp::types::SET_VARSIZE_4B;
using pgcpp::types::SET_VARSIZE_COMPRESSED;
using pgcpp::types::SET_VARSIZE_EXTERNAL;
using pgcpp::types::TextPGetDatum;
using pgcpp::types::varatt_external;
using pgcpp::types::VARATT_IS_COMPRESSED;
using pgcpp::types::VARATT_IS_EXTERNAL;
using pgcpp::types::VARATT_RAW_SIZE;
using pgcpp::types::VARDATA_4B;
using pgcpp::types::VARDATA_COMPRESSED;
using pgcpp::types::VARDATA_EXTERNAL;
using pgcpp::types::VARSIZE_ANY;
using pgcpp::types::VARSIZE_COMPRESSED_DATA;

// ===========================================================================
// pglz compression (simplified LZ77)
//
// Format:
//   [control byte] [item 0] [item 1] ... [item 7] [control byte] [item 0] ...
//
// Control byte: 8 bits, MSB first. For each bit:
//   0 = literal (1 byte follows)
//   1 = match (2 bytes follow: (length:4 | offset_hi:4), offset_lo:8)
//       length = nibble + 3 (range 3-18)
//       offset = (offset_hi << 8 | offset_lo) + 1 (range 1-4096)
// ===========================================================================

namespace {

// Hash table for finding 3-byte matches. Maps a 3-byte hash to the most
// recent position with that hash. Size is a power of 2 for fast modulo.
constexpr int kHashSize = 4096;
constexpr int kHashMask = kHashSize - 1;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 18;     // 4-bit length nibble + 3
constexpr int kMaxOffset = 4096;  // 12-bit offset

inline int hash3(const char* data, int pos) {
    unsigned b0 = static_cast<unsigned char>(data[pos]);
    unsigned b1 = static_cast<unsigned char>(data[pos + 1]);
    unsigned b2 = static_cast<unsigned char>(data[pos + 2]);
    return static_cast<int>(((b0 << 8) ^ (b1 << 4) ^ b2) & kHashMask);
}

// Find the longest match at `pos` looking back from `prev_pos`.
int find_match_len(const char* source, int prev_pos, int pos, int slen) {
    int max_len = std::min(kMaxMatch, slen - pos);
    int len = 0;
    while (len < max_len && source[prev_pos + len] == source[pos + len]) {
        ++len;
    }
    return len;
}

}  // namespace

bool pglz_compress(const char* source, int slen, char* dest, int* result_len) {
    // Hash table: hash → last position with that hash (-1 = none).
    std::vector<int> hash_table(kHashSize, -1);

    // Output buffer (we build into a temporary, then copy to dest).
    // Worst case: all literals = slen bytes + slen/8 control bytes.
    std::vector<char> output;
    output.reserve(slen + slen / 8 + 16);

    int pos = 0;
    while (pos < slen) {
        // Accumulate up to 8 items with a control byte.
        char ctrl = 0;
        std::vector<char> items;

        for (int bit = 0; bit < 8 && pos < slen; ++bit) {
            bool emitted_match = false;
            if (pos + kMinMatch <= slen) {
                int h = hash3(source, pos);
                int prev = hash_table[h];
                hash_table[h] = pos;

                if (prev >= 0 && (pos - prev) <= kMaxOffset) {
                    int mlen = find_match_len(source, prev, pos, slen);
                    if (mlen >= kMinMatch) {
                        // Emit a match.
                        ctrl |= (1 << (7 - bit));
                        int offset = pos - prev - 1;           // 0-based offset
                        int length_nibble = mlen - kMinMatch;  // 0-15
                        char b1 = static_cast<char>((length_nibble << 4) | ((offset >> 8) & 0x0F));
                        char b2 = static_cast<char>(offset & 0xFF);
                        items.push_back(b1);
                        items.push_back(b2);

                        // Update hash table for skipped positions.
                        for (int k = 1; k < mlen && pos + k + kMinMatch <= slen; ++k) {
                            hash_table[hash3(source, pos + k)] = pos + k;
                        }
                        pos += mlen;
                        emitted_match = true;
                    }
                }
            }

            if (!emitted_match) {
                // Emit a literal.
                items.push_back(source[pos]);
                ++pos;
            }
        }

        output.push_back(ctrl);
        output.insert(output.end(), items.begin(), items.end());
    }

    // Check if compression helped.
    int compressed_size = static_cast<int>(output.size());
    if (compressed_size >= slen) {
        return false;  // compression didn't help
    }

    std::memcpy(dest, output.data(), compressed_size);
    *result_len = compressed_size;
    return true;
}

int pglz_decompress(const char* source, int slen, char* dest, int destlen) {
    int src_pos = 0;
    int dest_pos = 0;

    while (src_pos < slen && dest_pos < destlen) {
        char ctrl = source[src_pos++];

        for (int bit = 7; bit >= 0 && src_pos < slen && dest_pos < destlen; --bit) {
            if (ctrl & (1 << bit)) {
                // Match: 2 bytes.
                if (src_pos + 2 > slen)
                    return -1;
                unsigned char b1 = static_cast<unsigned char>(source[src_pos++]);
                unsigned char b2 = static_cast<unsigned char>(source[src_pos++]);
                int length = (b1 >> 4) + kMinMatch;
                int offset = ((b1 & 0x0F) << 8 | b2) + 1;

                if (dest_pos - offset < 0)
                    return -1;
                if (dest_pos + length > destlen)
                    return -1;

                for (int k = 0; k < length; ++k) {
                    dest[dest_pos] = dest[dest_pos - offset];
                    ++dest_pos;
                }
            } else {
                // Literal: 1 byte.
                if (dest_pos >= destlen)
                    return -1;
                dest[dest_pos++] = source[src_pos++];
            }
        }
    }

    return dest_pos;
}

// ===========================================================================
// TOAST table management
// ===========================================================================

namespace {

// Build the tuple descriptor for a TOAST table.
TupleDesc make_toast_tupdesc() {
    std::vector<FormData_pg_attribute> attrs(3);

    // chunk_id: oid (uint32, byval, 4 bytes)
    attrs[0].attname = "chunk_id";
    attrs[0].atttypid = 26;  // OID type
    attrs[0].attlen = 4;
    attrs[0].attnum = 1;
    attrs[0].attbyval = true;
    attrs[0].attalign = AttAlign::kInt;
    attrs[0].attstorage = AttStorage::kPlain;

    // chunk_seq: int4
    attrs[1].attname = "chunk_seq";
    attrs[1].atttypid = 23;  // int4
    attrs[1].attlen = 4;
    attrs[1].attnum = 2;
    attrs[1].attbyval = true;
    attrs[1].attalign = AttAlign::kInt;
    attrs[1].attstorage = AttStorage::kPlain;

    // chunk_data: bytea (varlena)
    attrs[2].attname = "chunk_data";
    attrs[2].atttypid = 17;  // bytea
    attrs[2].attlen = -1;
    attrs[2].attnum = 3;
    attrs[2].attbyval = false;
    attrs[2].attalign = AttAlign::kInt;
    attrs[2].attstorage = AttStorage::kPlain;  // TOAST table data is never toasted

    return CreateTupleDesc(attrs);
}

// Create the catalog entries and storage for a TOAST table.
Oid create_toast_table(Relation parent_rel) {
    Catalog* cat = GetCatalog();
    Oid toast_oid = cat->AllocateOid();

    // Create pg_class entry for the TOAST table.
    auto* class_row = pgcpp::nodes::makePallocNode<FormData_pg_class>();
    class_row->oid = toast_oid;
    class_row->relname = "pg_toast_" + std::to_string(parent_rel->rd_id);
    class_row->relfilenode = toast_oid;
    class_row->relkind = RelKind::kToastValue;
    class_row->relpersistence = RelPersistence::kPermanent;
    class_row->relnatts = 3;
    cat->InsertClass(class_row);

    // Create pg_attribute entries.
    TupleDesc toast_tupdesc = make_toast_tupdesc();
    for (int i = 0; i < toast_tupdesc->natts; ++i) {
        auto* attr_row =
            pgcpp::nodes::makePallocNode<FormData_pg_attribute>(toast_tupdesc->attrs[i]);
        attr_row->attrelid = toast_oid;
        cat->InsertAttribute(attr_row);
    }

    // Create physical storage.
    RelationCreateStorage(toast_oid, false);

    // Update parent relation's reltoastrelid.
    FormData_pg_class parent_row = *parent_rel->rd_rel;
    parent_row.reltoastrelid = toast_oid;
    cat->UpdateClass(parent_rel->rd_id, &parent_row);

    // Also update the in-memory RelationData's rd_rel pointer.
    // The relcache will pick up the change on next open.
    const_cast<FormData_pg_class*>(parent_rel->rd_rel)->reltoastrelid = toast_oid;

    return toast_oid;
}

}  // namespace

Oid toast_get_or_create_table(Relation relation) {
    if (relation == nullptr || relation->rd_rel == nullptr) {
        return pgcpp::catalog::kInvalidOid;
    }

    // TOAST tables don't have their own TOAST tables.
    if (relation->rd_rel->relkind == RelKind::kToastValue) {
        return pgcpp::catalog::kInvalidOid;
    }

    Oid toast_relid = relation->rd_rel->reltoastrelid;
    if (toast_relid != pgcpp::catalog::kInvalidOid) {
        return toast_relid;
    }

    // Check if any column is varlena (could benefit from TOAST).
    bool has_varlena = false;
    if (relation->rd_att != nullptr) {
        for (int i = 0; i < relation->rd_att->natts; ++i) {
            if (relation->rd_att->attrs[i].attlen == -1) {
                has_varlena = true;
                break;
            }
        }
    }

    if (!has_varlena) {
        return pgcpp::catalog::kInvalidOid;
    }

    return create_toast_table(relation);
}

// ===========================================================================
// toast_save_datum — store a value in the TOAST table
// ===========================================================================

Datum toast_save_datum(Relation relation, const char* data, int data_len, bool compress) {
    Oid toast_relid = toast_get_or_create_table(relation);
    if (toast_relid == pgcpp::catalog::kInvalidOid) {
        ereport(pgcpp::error::LogLevel::kError, "toast_save_datum: no TOAST table available");
    }

    // Allocate a chunk_id for this value.
    Oid chunk_id = GetCatalog()->AllocateOid();

    // Optionally compress the data.
    int stored_len = data_len;
    const char* stored_data = data;
    bool is_compressed = false;

    if (compress && data_len > 32) {
        // Try compression (need extra byte for worst case).
        char* compressed = static_cast<char*>(palloc(data_len + data_len / 8 + 16));
        int comp_len = 0;
        if (pglz_compress(data, data_len, compressed, &comp_len)) {
            stored_data = compressed;
            stored_len = comp_len;
            is_compressed = true;
        }
        // If compression failed, stored_data/stored_len stay as original.
    }

    // Open the TOAST table and insert chunks.
    Relation toast_rel = RelationOpen(toast_relid);
    TupleDesc toast_tupdesc = toast_rel->rd_att;

    int chunk_seq = 0;
    int offset = 0;
    while (offset < stored_len) {
        int chunk_len = std::min(kToastMaxChunkSize, stored_len - offset);

        // Build the chunk row: [chunk_id, chunk_seq, chunk_data].
        Datum values[3];
        bool isnull[3] = {false, false, false};
        values[0] = Datum(chunk_id);
        values[1] = Datum(chunk_seq);

        // Create a varlena for the chunk data.
        int chunk_varlena_size = sizeof(int32_t) + chunk_len;
        char* chunk_varlena = static_cast<char*>(palloc(chunk_varlena_size));
        SET_VARSIZE_4B(chunk_varlena, chunk_varlena_size);
        std::memcpy(chunk_varlena + sizeof(int32_t), stored_data + offset, chunk_len);
        values[2] = TextPGetDatum(chunk_varlena);

        HeapTuple chunk_tup = heap_form_tuple(toast_tupdesc, values, isnull);
        heap_insert(toast_rel, chunk_tup);
        heap_freetuple(chunk_tup);

        offset += chunk_len;
        ++chunk_seq;
    }

    RelationClose(toast_rel);
    CommandCounterIncrement();

    // Build the varatt_external pointer.
    char* ext_ptr = static_cast<char*>(palloc(sizeof(varatt_external)));
    auto* ext = reinterpret_cast<varatt_external*>(ext_ptr);
    ext->va_header = kVarAttExternal;
    // va_rawsize convention: positive = compressed (value = original size),
    // negative = not compressed (|value| = original size).
    ext->va_rawsize = is_compressed ? data_len : -data_len;
    ext->va_valueid = chunk_id;
    ext->va_toastrelid = toast_relid;

    return TextPGetDatum(ext_ptr);
}

// ===========================================================================
// toast_get_datum — fetch and reassemble a TOASTed value
// ===========================================================================

Datum toast_get_datum(Relation relation, Datum value) {
    (void)relation;  // TOAST table OID is extracted from the external pointer
    const char* text = DatumGetTextP(value);
    const varatt_external* ext = VARDATA_EXTERNAL(text);

    // Read the va_rawsize: positive = compressed, negative = uncompressed.
    int32_t rawsize = ext->va_rawsize;
    bool is_compressed = (rawsize > 0);
    int original_len = is_compressed ? rawsize : -rawsize;

    // Open the TOAST table and scan for chunks.
    Relation toast_rel = RelationOpen(ext->va_toastrelid);
    TupleDesc toast_tupdesc = toast_rel->rd_att;

    // Collect chunks.
    std::vector<std::pair<int, std::vector<char>>> chunks;
    HeapScanDesc scan = heap_beginscan(toast_rel, nullptr);
    HeapTuple tup = nullptr;
    while ((tup = heap_getnext(scan)) != nullptr) {
        Datum values[3];
        bool isnull[3];
        heap_deform_tuple(tup, toast_tupdesc, values, isnull);

        if (isnull[0] || values[0] != Datum(ext->va_valueid)) {
            continue;  // not our chunk
        }

        int seq = static_cast<int>(values[1]);
        const char* chunk_text = DatumGetTextP(values[2]);
        int chunk_data_len = static_cast<int>(VARSIZE_ANY(chunk_text) - sizeof(int32_t));

        std::vector<char> chunk_data(chunk_text + sizeof(int32_t),
                                     chunk_text + sizeof(int32_t) + chunk_data_len);
        chunks.emplace_back(seq, std::move(chunk_data));
    }
    heap_endscan(scan);
    RelationClose(toast_rel);

    // Sort chunks by sequence number.
    std::sort(chunks.begin(), chunks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Concatenate chunk data.
    std::vector<char> stored_data;
    for (auto& [seq, data] : chunks) {
        stored_data.insert(stored_data.end(), data.begin(), data.end());
    }

    // Decompress if needed.
    std::vector<char> result_data;
    if (is_compressed) {
        result_data.resize(original_len);
        int decompressed = pglz_decompress(stored_data.data(), static_cast<int>(stored_data.size()),
                                           result_data.data(), original_len);
        if (decompressed != original_len) {
            ereport(pgcpp::error::LogLevel::kError, "toast_get_datum: decompression failed");
        }
    } else {
        result_data = std::move(stored_data);
    }

    // Build a normal varlena Datum.
    int total_size = sizeof(int32_t) + static_cast<int>(result_data.size());
    char* result = static_cast<char*>(palloc(total_size));
    SET_VARSIZE_4B(result, total_size);
    std::memcpy(result + sizeof(int32_t), result_data.data(), result_data.size());

    return TextPGetDatum(result);
}

// ===========================================================================
// toast_delete_datum — delete TOAST chunks for a value
// ===========================================================================

void toast_delete_datum(Relation relation, Datum value) {
    (void)relation;  // TOAST table OID is extracted from the external pointer
    const char* text = DatumGetTextP(value);
    if (!VARATT_IS_EXTERNAL(text)) {
        return;  // not an external pointer
    }

    const varatt_external* ext = VARDATA_EXTERNAL(text);
    Relation toast_rel = RelationOpen(ext->va_toastrelid);
    TupleDesc toast_tupdesc = toast_rel->rd_att;

    // Scan and delete matching chunks.
    HeapScanDesc scan = heap_beginscan(toast_rel, nullptr);
    HeapTuple tup = nullptr;
    while ((tup = heap_getnext(scan)) != nullptr) {
        Datum values[3];
        bool isnull[3];
        heap_deform_tuple(tup, toast_tupdesc, values, isnull);

        if (isnull[0] || values[0] != Datum(ext->va_valueid)) {
            continue;  // not our chunk
        }

        ItemPointerData tid = tup->t_self;
        heap_delete(toast_rel, tid);
    }
    heap_endscan(scan);
    RelationClose(toast_rel);
    CommandCounterIncrement();
}

// ===========================================================================
// toast_insert_or_update — toast large values before insert
// ===========================================================================

void toast_insert_or_update(Relation relation, Datum* values, const bool* isnull,
                            TupleDesc tupdesc) {
    if (relation == nullptr || relation->rd_rel == nullptr) {
        return;
    }

    // Don't process TOAST tables (prevents infinite recursion).
    if (relation->rd_rel->relkind == RelKind::kToastValue) {
        return;
    }

    for (int i = 0; i < tupdesc->natts; ++i) {
        if (isnull != nullptr && isnull[i]) {
            continue;
        }

        const auto& attr = tupdesc->attrs[i];
        if (attr.attlen != -1) {
            continue;  // not a varlena column
        }

        AttStorage storage = attr.attstorage;
        if (storage == AttStorage::kPlain) {
            continue;  // plain storage — never toast
        }

        const char* text = DatumGetTextP(values[i]);
        int data_len = static_cast<int>(VARSIZE_ANY(text) - sizeof(int32_t));

        // Only toast values that exceed the threshold.
        if (data_len <= kToastThreshold) {
            continue;
        }

        // Step 1: Try compression (for 'x' extended and 'm' main).
        if (storage == AttStorage::kExtended || storage == AttStorage::kMain) {
            char* compressed = static_cast<char*>(palloc(data_len + data_len / 8 + 16));
            int comp_len = 0;
            if (pglz_compress(VARDATA_4B(const_cast<char*>(text)), data_len, compressed,
                              &comp_len)) {
                // Compression succeeded. Check if it's still too large.
                int comp_varlena_size = sizeof(int32_t) + sizeof(int32_t) + comp_len;
                if (storage == AttStorage::kMain && comp_varlena_size <= kToastThreshold) {
                    // Keep compressed inline (kMain: try compress but keep inline).
                    char* inline_val = static_cast<char*>(palloc(comp_varlena_size));
                    SET_VARSIZE_COMPRESSED(inline_val, comp_varlena_size);
                    int32_t raw_size = data_len;
                    std::memcpy(inline_val + sizeof(int32_t), &raw_size, sizeof(raw_size));
                    std::memcpy(inline_val + sizeof(int32_t) + sizeof(int32_t), compressed,
                                comp_len);
                    values[i] = TextPGetDatum(inline_val);
                    continue;
                }
                // For 'x' extended: if compressed value still > threshold, move external.
                if (storage == AttStorage::kExtended && comp_varlena_size <= kToastThreshold) {
                    // Compressed inline is small enough.
                    char* inline_val = static_cast<char*>(palloc(comp_varlena_size));
                    SET_VARSIZE_COMPRESSED(inline_val, comp_varlena_size);
                    int32_t raw_size = data_len;
                    std::memcpy(inline_val + sizeof(int32_t), &raw_size, sizeof(raw_size));
                    std::memcpy(inline_val + sizeof(int32_t) + sizeof(int32_t), compressed,
                                comp_len);
                    values[i] = TextPGetDatum(inline_val);
                    continue;
                }
                // Still too large after compression — move to TOAST table.
                // Pass the compressed data to toast_save_datum.
                Datum ext = toast_save_datum(relation, compressed, comp_len, false);
                values[i] = ext;
                continue;
            }
        }

        // Step 2: Move to TOAST table (for 'x' extended and 'e' external).
        if (storage == AttStorage::kExtended || storage == AttStorage::kExternal) {
            // Try compression first, store compressed externally.
            bool do_compress = (storage == AttStorage::kExtended);
            Datum ext = toast_save_datum(relation, VARDATA_4B(const_cast<char*>(text)), data_len,
                                         do_compress);
            values[i] = ext;
        }
    }
}

// ===========================================================================
// detoast_attr — detoast a varlena Datum
// ===========================================================================

Datum detoast_attr(Datum value) {
    const char* text = DatumGetTextP(value);

    // External TOAST pointer: fetch from TOAST table.
    if (VARATT_IS_EXTERNAL(text)) {
        const varatt_external* ext = VARDATA_EXTERNAL(text);
        Relation toast_rel = RelationOpen(ext->va_toastrelid);
        Datum result = toast_get_datum(toast_rel, value);
        RelationClose(toast_rel);
        return result;
    }

    // Compressed inline: decompress.
    if (VARATT_IS_COMPRESSED(text)) {
        int raw_size = VARATT_RAW_SIZE(text);
        int comp_data_size = VARSIZE_COMPRESSED_DATA(text);
        const char* comp_data = VARDATA_COMPRESSED(text);

        char* dest = static_cast<char*>(palloc(sizeof(int32_t) + raw_size));
        SET_VARSIZE_4B(dest, static_cast<uint32_t>(sizeof(int32_t) + raw_size));

        int decompressed =
            pglz_decompress(comp_data, comp_data_size, dest + sizeof(int32_t), raw_size);
        if (decompressed != raw_size) {
            ereport(pgcpp::error::LogLevel::kError, "detoast_attr: decompression failed");
        }
        return TextPGetDatum(dest);
    }

    // Normal varlena: return as-is.
    return value;
}

// ===========================================================================
// toast_delete_tuple — delete TOAST chunks for all external columns
// ===========================================================================

void toast_delete_tuple(Relation relation, Datum* values, const bool* isnull, TupleDesc tupdesc) {
    if (relation == nullptr || relation->rd_rel == nullptr) {
        return;
    }

    // Don't process TOAST tables.
    if (relation->rd_rel->relkind == RelKind::kToastValue) {
        return;
    }

    for (int i = 0; i < tupdesc->natts; ++i) {
        if (isnull != nullptr && isnull[i]) {
            continue;
        }
        if (tupdesc->attrs[i].attlen != -1) {
            continue;
        }

        const char* text = DatumGetTextP(values[i]);
        if (VARATT_IS_EXTERNAL(text)) {
            toast_delete_datum(relation, values[i]);
        }
    }
}

}  // namespace pgcpp::access
