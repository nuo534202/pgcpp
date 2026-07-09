// toast.h — TOAST (The Oversized-Attribute Storage Technique) API.
//
// Converted from PostgreSQL 15's src/include/access/toast_helper.h and
// src/backend/access/common/toast_internals.c.
//
// TOAST handles varlena values that are too large to fit inline in a heap
// page (~8KB). Two mechanisms:
//
//   1. Compression (pglz): compress the value inline if the column's
//      attstorage is 'x' (extended) or 'm' (main).
//
//   2. Out-of-line storage: if the value is still too large after
//      compression (or if attstorage is 'e'), move it to a separate TOAST
//      table and store a 16-byte varatt_external pointer inline.
//
// The TOAST table has three columns:
//   chunk_id   (oid)   — identifies the toasted value
//   chunk_seq  (int4)  — sequence number (0, 1, 2, ...)
//   chunk_data (bytea) — a chunk of the value (up to kToastMaxChunkSize bytes)
//
// Key entry points:
//   toast_insert_or_update — toast large values before heap_insert
//   toast_delete_datum     — delete TOAST chunks when a tuple is deleted
//   detoast_attr           — detoast a varlena Datum on read
#pragma once

#include <cstdint>

#include "access/rel.hpp"
#include "types/datum.hpp"

namespace pgcpp::access {

// --- TOAST constants ---
//
// kToastThreshold: values larger than this are candidates for compression
//   and/or out-of-line storage. PostgreSQL uses TOAST_TUPLE_THRESHOLD (~2KB).
// kToastMaxChunkSize: maximum chunk data size. Must be small enough that
//   a TOAST chunk row fits in a single heap page (8KB). PostgreSQL uses
//   TOAST_MAX_CHUNK_SIZE (~1996 bytes).
constexpr int kToastThreshold = 2000;
constexpr int kToastMaxChunkSize = 1996;

// --- Compression (pglz) ---
//
// pglz_compress: compress source into dest. Returns the compressed size
// via *result_len, or false if compression didn't help (result >= source).
// dest must have at least slen + 1 bytes.
bool pglz_compress(const char* source, int slen, char* dest, int* result_len);

// pglz_decompress: decompress source (slen bytes) into dest (up to destlen).
// Returns the number of bytes written, or -1 on error.
int pglz_decompress(const char* source, int slen, char* dest, int destlen);

// --- TOAST table management ---

// toast_get_or_create_table — get the TOAST table OID for a relation.
// Creates the TOAST table (catalog entry + storage) if it doesn't exist
// and the relation has varlena columns. Returns kInvalidOid if no TOAST
// table is needed or could be created.
pgcpp::catalog::Oid toast_get_or_create_table(Relation relation);

// toast_save_datum — store a value in the TOAST table, returning an
// external pointer (varatt_external) as a palloc'd varlena Datum.
// If compressed is true, the data is first compressed.
pgcpp::types::Datum toast_save_datum(Relation relation, const char* data, int data_len,
                                     bool compress);

// toast_get_datum — fetch and reassemble a TOASTed value, optionally
// decompressing. Returns a palloc'd normal (uncompressed, inline) varlena.
pgcpp::types::Datum toast_get_datum(Relation relation, pgcpp::types::Datum value);

// toast_delete_datum — delete all TOAST chunks for the given external
// pointer. No-op if the value is not external.
void toast_delete_datum(Relation relation, pgcpp::types::Datum value);

// --- High-level TOAST operations ---

// toast_insert_or_update — examine all varlena columns in the tuple and
// compress/toast values that exceed kToastThreshold. Modifies values[]
// in place (replacing large Datums with compressed or external pointers).
// The original Datums are palloc'd and must be freed by the caller.
void toast_insert_or_update(Relation relation, pgcpp::types::Datum* values, const bool* isnull,
                            TupleDesc tupdesc);

// detoast_attr — if the varlena Datum is external or compressed, detoast
// it (fetch from TOAST table + decompress). Returns a palloc'd normal
// varlena Datum. If the value is already normal, returns it unchanged.
// The TOAST table OID is extracted from the varatt_external pointer,
// so no Relation parameter is needed.
pgcpp::types::Datum detoast_attr(pgcpp::types::Datum value);

// toast_delete_tuple — delete TOAST chunks for all external varlena
// columns in a tuple. Called by heap_delete before removing the tuple.
void toast_delete_tuple(Relation relation, pgcpp::types::Datum* values, const bool* isnull,
                        TupleDesc tupdesc);

}  // namespace pgcpp::access
