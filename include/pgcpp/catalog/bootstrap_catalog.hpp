#pragma once

namespace mytoydb::catalog {

class Catalog;

// BootstrapCatalog populates the given catalog with PostgreSQL's built-in
// operators, functions, casts, aggregates, and collations. Rows are allocated
// via palloc in the current memory context; the catalog takes ownership.
void BootstrapCatalog(Catalog* cat);

}  // namespace mytoydb::catalog
