// file_fdw.h — file_fdw handler: reads CSV files as foreign tables.
//
// Converted from PostgreSQL 15's contrib/file_fdw/file_fdw.c.
//
// file_fdw is a reference FDW handler that reads data from CSV (comma-
// separated values) files. It demonstrates the FDW scan lifecycle:
//   BeginForeignScan  — opens the CSV file specified in the foreign table's
//                       "filename" option.
//   IterateForeignScan — reads the next line, splits on commas, converts each
//                        field to the column's type, and fills the scan slot.
//   ReScanForeignScan — rewinds to the beginning of the file.
//   EndForeignScan    — closes the file.
//
// Limitations vs PostgreSQL's file_fdw:
//   - CSV only (no fixed-width, binary, or custom delimiters).
//   - No header skipping (the first line is data).
//   - No quoting/escaping (fields cannot contain commas or newlines).
//   - NULL is represented as an empty field.
//   - No WHERE pushdown (all rows are read and filtered by the executor).
#pragma once

#include <string>

namespace pgcpp::foreign {

// RegisterFileFdw — register the file_fdw handler under the name "file_fdw".
// Call once at startup (or in tests before creating foreign servers that
// use file_fdw). Safe to call multiple times only after ClearFdwRegistry().
void RegisterFileFdw();

}  // namespace pgcpp::foreign
