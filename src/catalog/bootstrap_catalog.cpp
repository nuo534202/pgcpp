// bootstrap_catalog.cpp — populates the catalog with PostgreSQL built-ins.
//
// Converts PostgreSQL's bootstrap catalog data (pg_operator.dat, pg_proc.dat,
// pg_cast.dat, pg_aggregate.dat, pg_collation.dat) to C++20. Each row is
// palloc-allocated and placement-constructed, then handed to the Catalog,
// preserving PostgreSQL's "rows live in a long-lived memory context" model.

#include "mytoydb/catalog/bootstrap_catalog.h"

#include <new>
#include <vector>

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/pg_aggregate.h"
#include "mytoydb/catalog/pg_cast.h"
#include "mytoydb/catalog/pg_collation.h"
#include "mytoydb/catalog/pg_operator.h"
#include "mytoydb/catalog/pg_proc.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/types/datum.h"

namespace mytoydb::catalog {

namespace {

// PostgreSQL UNKNOWNOID (the "unknown" pseudo-type). Used here as the argument
// type of input functions and the return type of output functions, standing in
// for cstring (CSTRINGOID=2275) which MyToyDB does not yet model.
constexpr Oid kUnknownOid = 705;

// Type OID aliases (shorter names within this translation unit).
using mytoydb::types::kBoolOid;
using mytoydb::types::kDateOid;
using mytoydb::types::kFloat4Oid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kNumericOid;
using mytoydb::types::kTextOid;
using mytoydb::types::kTimestampOid;
using mytoydb::types::kTimestamptzOid;
using mytoydb::types::kVarcharOid;

// Function OIDs (PostgreSQL pg_proc_d.h constants), used as operator oprcode.
constexpr Oid F_INT4EQ = 350;
constexpr Oid F_INT4NE = 352;
constexpr Oid F_INT4LT = 97;
constexpr Oid F_INT4LE = 523;
constexpr Oid F_INT4GT = 521;
constexpr Oid F_INT4GE = 525;
constexpr Oid F_INT4PL = 551;
constexpr Oid F_INT4MI = 550;
constexpr Oid F_INT4MUL = 552;
constexpr Oid F_INT4DIV = 553;
// Arithmetic function OIDs for other numeric types. Values are chosen to
// avoid conflicts with existing constants; exact PostgreSQL pg_proc OIDs
// are not critical for parse analysis (the executor is not yet built).
constexpr Oid F_INT2PL = 600;
constexpr Oid F_INT2MI = 601;
constexpr Oid F_INT2MUL = 602;
constexpr Oid F_INT2DIV = 603;
constexpr Oid F_INT8PL = 604;
constexpr Oid F_INT8MI = 605;
constexpr Oid F_INT8MUL = 606;
constexpr Oid F_INT8DIV = 607;
constexpr Oid F_FLOAT4PL = 608;
constexpr Oid F_FLOAT4MI = 609;
constexpr Oid F_FLOAT4MUL = 610;
constexpr Oid F_FLOAT4DIV = 611;
constexpr Oid F_FLOAT8PL = 612;
constexpr Oid F_FLOAT8MI = 613;
constexpr Oid F_FLOAT8MUL = 614;
constexpr Oid F_FLOAT8DIV = 615;
constexpr Oid F_INT8EQ = 410;
constexpr Oid F_INT8NE = 411;
constexpr Oid F_INT8LT = 412;
constexpr Oid F_INT8LE = 414;
constexpr Oid F_INT8GT = 413;
constexpr Oid F_INT8GE = 415;
constexpr Oid F_INT2EQ = 418;
constexpr Oid F_INT2NE = 419;
constexpr Oid F_INT2LT = 95;
constexpr Oid F_INT2LE = 522;
constexpr Oid F_INT2GT = 524;
constexpr Oid F_INT2GE = 527;
constexpr Oid F_FLOAT4EQ = 620;
constexpr Oid F_FLOAT4NE = 621;
constexpr Oid F_FLOAT4LT = 622;
constexpr Oid F_FLOAT4LE = 624;
constexpr Oid F_FLOAT4GT = 623;
constexpr Oid F_FLOAT4GE = 625;
constexpr Oid F_FLOAT8EQ = 414;
constexpr Oid F_FLOAT8NE = 415;
constexpr Oid F_FLOAT8LT = 672;
constexpr Oid F_FLOAT8LE = 673;
constexpr Oid F_FLOAT8GT = 674;
constexpr Oid F_FLOAT8GE = 675;
constexpr Oid F_TEXTEQ = 98;
constexpr Oid F_TEXTNE = 531;
constexpr Oid F_TEXT_LT = 664;
constexpr Oid F_TEXT_LE = 665;
constexpr Oid F_TEXT_GT = 666;
constexpr Oid F_TEXT_GE = 667;
constexpr Oid F_TEXTLIKE = 850;
constexpr Oid F_TEXTNLIKE = 851;
constexpr Oid F_TEXTICLIKE = 1627;
constexpr Oid F_TEXTICNLIKE = 1628;
constexpr Oid F_BOOLEQ = 58;
constexpr Oid F_BOOLNE = 85;
constexpr Oid F_BOOLLT = 58;
constexpr Oid F_BOOLLE = 58;
constexpr Oid F_BOOLGT = 85;
constexpr Oid F_BOOLGE = 85;

// Date comparison function OIDs (PostgreSQL pg_proc_d.h).
constexpr Oid F_DATE_EQ = 1092;
constexpr Oid F_DATE_NE = 1093;
constexpr Oid F_DATE_LT = 1094;
constexpr Oid F_DATE_LE = 1095;
constexpr Oid F_DATE_GT = 1096;
constexpr Oid F_DATE_GE = 1097;

// Timestamp comparison function OIDs (PostgreSQL pg_proc_d.h).
constexpr Oid F_TIMESTAMP_EQ = 1320;
constexpr Oid F_TIMESTAMP_NE = 1321;
constexpr Oid F_TIMESTAMP_LT = 1322;
constexpr Oid F_TIMESTAMP_LE = 1323;
constexpr Oid F_TIMESTAMP_GT = 1324;
constexpr Oid F_TIMESTAMP_GE = 1325;

}  // namespace

// --- Row construction helpers ---
//
// Each helper palloc-ates a row, placement-constructs it, and fills in the
// fields callers care about. Remaining fields keep their struct defaults.

static FormData_pg_operator* MakeOp(Oid oid, const char* name, Oid left, Oid right, Oid result,
                                    Oid code, bool canmerge = false, bool canhash = false) {
    void* mem = mytoydb::memory::palloc(sizeof(FormData_pg_operator));
    auto* op = new (mem) FormData_pg_operator();
    op->oid = oid;
    op->oprname = name;
    op->oprleft = left;
    op->oprright = right;
    op->oprresult = result;
    op->oprcode = code;
    op->oprcanmerge = canmerge;
    op->oprcanhash = canhash;
    return op;
}

static FormData_pg_proc* MakeProc(Oid oid, const char* name, Oid rettype, std::vector<Oid> argtypes,
                                  ProKind kind = ProKind::kFunction, bool retset = false) {
    void* mem = mytoydb::memory::palloc(sizeof(FormData_pg_proc));
    auto* proc = new (mem) FormData_pg_proc();
    proc->oid = oid;
    proc->proname = name;
    proc->prorettype = rettype;
    proc->proargtypes = std::move(argtypes);
    proc->pronargs = static_cast<int16_t>(proc->proargtypes.size());
    proc->prokind = kind;
    proc->proretset = retset;
    return proc;
}

static FormData_pg_cast* MakeCast(Oid source, Oid target, Oid func, CastContext ctx,
                                  CastMethod method) {
    void* mem = mytoydb::memory::palloc(sizeof(FormData_pg_cast));
    auto* cast = new (mem) FormData_pg_cast();
    cast->castsource = source;
    cast->casttarget = target;
    cast->castfunc = func;
    cast->castcontext = ctx;
    cast->castmethod = method;
    return cast;
}

static FormData_pg_aggregate* MakeAgg(Oid aggfnoid, Oid transfn, Oid finalfn, Oid transtype,
                                      const char* initval = "") {
    void* mem = mytoydb::memory::palloc(sizeof(FormData_pg_aggregate));
    auto* agg = new (mem) FormData_pg_aggregate();
    agg->aggfnoid = aggfnoid;
    agg->aggtransfn = transfn;
    agg->aggfinalfn = finalfn;
    agg->aggtranstype = transtype;
    agg->agginitval = initval;
    return agg;
}

static FormData_pg_collation* MakeCollation(Oid oid, const char* name, CollProvider provider) {
    void* mem = mytoydb::memory::palloc(sizeof(FormData_pg_collation));
    auto* coll = new (mem) FormData_pg_collation();
    coll->oid = oid;
    coll->collname = name;
    coll->collprovider = provider;
    // collisdeterministic (true) and collencoding (-1) keep their defaults.
    return coll;
}

void BootstrapCatalog(Catalog* cat) {
    // --- Operators ---
    //
    // int4 uses PostgreSQL's canonical operator OIDs. For the other types the
    // operator OIDs are left as kInvalidOid and the catalog assigns fresh OIDs
    // (the underlying function OIDs, oprcode, are always set from the F_*
    // constants above). Comparison operators return bool; arithmetic returns
    // the operand type.

    // int4 comparison and arithmetic.
    cat->InsertOperator(MakeOp(96, "=", kInt4Oid, kInt4Oid, kBoolOid, F_INT4EQ));
    cat->InsertOperator(MakeOp(80, "<>", kInt4Oid, kInt4Oid, kBoolOid, F_INT4NE));
    cat->InsertOperator(MakeOp(97, "<", kInt4Oid, kInt4Oid, kBoolOid, F_INT4LT));
    cat->InsertOperator(MakeOp(523, "<=", kInt4Oid, kInt4Oid, kBoolOid, F_INT4LE));
    cat->InsertOperator(MakeOp(521, ">", kInt4Oid, kInt4Oid, kBoolOid, F_INT4GT));
    cat->InsertOperator(MakeOp(525, ">=", kInt4Oid, kInt4Oid, kBoolOid, F_INT4GE));
    cat->InsertOperator(MakeOp(551, "+", kInt4Oid, kInt4Oid, kInt4Oid, F_INT4PL));
    cat->InsertOperator(MakeOp(550, "-", kInt4Oid, kInt4Oid, kInt4Oid, F_INT4MI));
    cat->InsertOperator(MakeOp(552, "*", kInt4Oid, kInt4Oid, kInt4Oid, F_INT4MUL));
    cat->InsertOperator(MakeOp(553, "/", kInt4Oid, kInt4Oid, kInt4Oid, F_INT4DIV));

    // int8 comparison and arithmetic.
    cat->InsertOperator(MakeOp(kInvalidOid, "=", kInt8Oid, kInt8Oid, kBoolOid, F_INT8EQ));
    cat->InsertOperator(MakeOp(kInvalidOid, "<>", kInt8Oid, kInt8Oid, kBoolOid, F_INT8NE));
    cat->InsertOperator(MakeOp(kInvalidOid, "<", kInt8Oid, kInt8Oid, kBoolOid, F_INT8LT));
    cat->InsertOperator(MakeOp(kInvalidOid, "<=", kInt8Oid, kInt8Oid, kBoolOid, F_INT8LE));
    cat->InsertOperator(MakeOp(kInvalidOid, ">", kInt8Oid, kInt8Oid, kBoolOid, F_INT8GT));
    cat->InsertOperator(MakeOp(kInvalidOid, ">=", kInt8Oid, kInt8Oid, kBoolOid, F_INT8GE));
    cat->InsertOperator(MakeOp(kInvalidOid, "+", kInt8Oid, kInt8Oid, kInt8Oid, F_INT8PL));
    cat->InsertOperator(MakeOp(kInvalidOid, "-", kInt8Oid, kInt8Oid, kInt8Oid, F_INT8MI));
    cat->InsertOperator(MakeOp(kInvalidOid, "*", kInt8Oid, kInt8Oid, kInt8Oid, F_INT8MUL));
    cat->InsertOperator(MakeOp(kInvalidOid, "/", kInt8Oid, kInt8Oid, kInt8Oid, F_INT8DIV));

    // int2 comparison and arithmetic.
    cat->InsertOperator(MakeOp(kInvalidOid, "=", kInt2Oid, kInt2Oid, kBoolOid, F_INT2EQ));
    cat->InsertOperator(MakeOp(kInvalidOid, "<>", kInt2Oid, kInt2Oid, kBoolOid, F_INT2NE));
    cat->InsertOperator(MakeOp(kInvalidOid, "<", kInt2Oid, kInt2Oid, kBoolOid, F_INT2LT));
    cat->InsertOperator(MakeOp(kInvalidOid, "<=", kInt2Oid, kInt2Oid, kBoolOid, F_INT2LE));
    cat->InsertOperator(MakeOp(kInvalidOid, ">", kInt2Oid, kInt2Oid, kBoolOid, F_INT2GT));
    cat->InsertOperator(MakeOp(kInvalidOid, ">=", kInt2Oid, kInt2Oid, kBoolOid, F_INT2GE));
    cat->InsertOperator(MakeOp(kInvalidOid, "+", kInt2Oid, kInt2Oid, kInt2Oid, F_INT2PL));
    cat->InsertOperator(MakeOp(kInvalidOid, "-", kInt2Oid, kInt2Oid, kInt2Oid, F_INT2MI));
    cat->InsertOperator(MakeOp(kInvalidOid, "*", kInt2Oid, kInt2Oid, kInt2Oid, F_INT2MUL));
    cat->InsertOperator(MakeOp(kInvalidOid, "/", kInt2Oid, kInt2Oid, kInt2Oid, F_INT2DIV));

    // float4 comparison and arithmetic.
    cat->InsertOperator(MakeOp(kInvalidOid, "=", kFloat4Oid, kFloat4Oid, kBoolOid, F_FLOAT4EQ));
    cat->InsertOperator(MakeOp(kInvalidOid, "<>", kFloat4Oid, kFloat4Oid, kBoolOid, F_FLOAT4NE));
    cat->InsertOperator(MakeOp(kInvalidOid, "<", kFloat4Oid, kFloat4Oid, kBoolOid, F_FLOAT4LT));
    cat->InsertOperator(MakeOp(kInvalidOid, "<=", kFloat4Oid, kFloat4Oid, kBoolOid, F_FLOAT4LE));
    cat->InsertOperator(MakeOp(kInvalidOid, ">", kFloat4Oid, kFloat4Oid, kBoolOid, F_FLOAT4GT));
    cat->InsertOperator(MakeOp(kInvalidOid, ">=", kFloat4Oid, kFloat4Oid, kBoolOid, F_FLOAT4GE));
    cat->InsertOperator(MakeOp(kInvalidOid, "+", kFloat4Oid, kFloat4Oid, kFloat4Oid, F_FLOAT4PL));
    cat->InsertOperator(MakeOp(kInvalidOid, "-", kFloat4Oid, kFloat4Oid, kFloat4Oid, F_FLOAT4MI));
    cat->InsertOperator(MakeOp(kInvalidOid, "*", kFloat4Oid, kFloat4Oid, kFloat4Oid, F_FLOAT4MUL));
    cat->InsertOperator(MakeOp(kInvalidOid, "/", kFloat4Oid, kFloat4Oid, kFloat4Oid, F_FLOAT4DIV));

    // float8 comparison and arithmetic.
    cat->InsertOperator(MakeOp(kInvalidOid, "=", kFloat8Oid, kFloat8Oid, kBoolOid, F_FLOAT8EQ));
    cat->InsertOperator(MakeOp(kInvalidOid, "<>", kFloat8Oid, kFloat8Oid, kBoolOid, F_FLOAT8NE));
    cat->InsertOperator(MakeOp(kInvalidOid, "<", kFloat8Oid, kFloat8Oid, kBoolOid, F_FLOAT8LT));
    cat->InsertOperator(MakeOp(kInvalidOid, "<=", kFloat8Oid, kFloat8Oid, kBoolOid, F_FLOAT8LE));
    cat->InsertOperator(MakeOp(kInvalidOid, ">", kFloat8Oid, kFloat8Oid, kBoolOid, F_FLOAT8GT));
    cat->InsertOperator(MakeOp(kInvalidOid, ">=", kFloat8Oid, kFloat8Oid, kBoolOid, F_FLOAT8GE));
    cat->InsertOperator(MakeOp(kInvalidOid, "+", kFloat8Oid, kFloat8Oid, kFloat8Oid, F_FLOAT8PL));
    cat->InsertOperator(MakeOp(kInvalidOid, "-", kFloat8Oid, kFloat8Oid, kFloat8Oid, F_FLOAT8MI));
    cat->InsertOperator(MakeOp(kInvalidOid, "*", kFloat8Oid, kFloat8Oid, kFloat8Oid, F_FLOAT8MUL));
    cat->InsertOperator(MakeOp(kInvalidOid, "/", kFloat8Oid, kFloat8Oid, kFloat8Oid, F_FLOAT8DIV));

    // text comparison and pattern matching (~~ = LIKE, !~~ = NOT LIKE,
    // ~~* = ILIKE, !~~* = NOT ILIKE).
    cat->InsertOperator(MakeOp(kInvalidOid, "=", kTextOid, kTextOid, kBoolOid, F_TEXTEQ));
    cat->InsertOperator(MakeOp(kInvalidOid, "<>", kTextOid, kTextOid, kBoolOid, F_TEXTNE));
    cat->InsertOperator(MakeOp(kInvalidOid, "<", kTextOid, kTextOid, kBoolOid, F_TEXT_LT));
    cat->InsertOperator(MakeOp(kInvalidOid, "<=", kTextOid, kTextOid, kBoolOid, F_TEXT_LE));
    cat->InsertOperator(MakeOp(kInvalidOid, ">", kTextOid, kTextOid, kBoolOid, F_TEXT_GT));
    cat->InsertOperator(MakeOp(kInvalidOid, ">=", kTextOid, kTextOid, kBoolOid, F_TEXT_GE));
    cat->InsertOperator(MakeOp(kInvalidOid, "~~", kTextOid, kTextOid, kBoolOid, F_TEXTLIKE));
    cat->InsertOperator(MakeOp(kInvalidOid, "!~~", kTextOid, kTextOid, kBoolOid, F_TEXTNLIKE));
    cat->InsertOperator(MakeOp(kInvalidOid, "~~*", kTextOid, kTextOid, kBoolOid, F_TEXTICLIKE));
    cat->InsertOperator(MakeOp(kInvalidOid, "!~~*", kTextOid, kTextOid, kBoolOid, F_TEXTICNLIKE));

    // bool comparison.
    cat->InsertOperator(MakeOp(kInvalidOid, "=", kBoolOid, kBoolOid, kBoolOid, F_BOOLEQ));
    cat->InsertOperator(MakeOp(kInvalidOid, "<>", kBoolOid, kBoolOid, kBoolOid, F_BOOLNE));
    cat->InsertOperator(MakeOp(kInvalidOid, "<", kBoolOid, kBoolOid, kBoolOid, F_BOOLLT));
    cat->InsertOperator(MakeOp(kInvalidOid, "<=", kBoolOid, kBoolOid, kBoolOid, F_BOOLLE));
    cat->InsertOperator(MakeOp(kInvalidOid, ">", kBoolOid, kBoolOid, kBoolOid, F_BOOLGT));
    cat->InsertOperator(MakeOp(kInvalidOid, ">=", kBoolOid, kBoolOid, kBoolOid, F_BOOLGE));

    // date comparison.
    cat->InsertOperator(MakeOp(kInvalidOid, "=", kDateOid, kDateOid, kBoolOid, F_DATE_EQ));
    cat->InsertOperator(MakeOp(kInvalidOid, "<>", kDateOid, kDateOid, kBoolOid, F_DATE_NE));
    cat->InsertOperator(MakeOp(kInvalidOid, "<", kDateOid, kDateOid, kBoolOid, F_DATE_LT));
    cat->InsertOperator(MakeOp(kInvalidOid, "<=", kDateOid, kDateOid, kBoolOid, F_DATE_LE));
    cat->InsertOperator(MakeOp(kInvalidOid, ">", kDateOid, kDateOid, kBoolOid, F_DATE_GT));
    cat->InsertOperator(MakeOp(kInvalidOid, ">=", kDateOid, kDateOid, kBoolOid, F_DATE_GE));

    // timestamp comparison.
    cat->InsertOperator(
        MakeOp(kInvalidOid, "=", kTimestampOid, kTimestampOid, kBoolOid, F_TIMESTAMP_EQ));
    cat->InsertOperator(
        MakeOp(kInvalidOid, "<>", kTimestampOid, kTimestampOid, kBoolOid, F_TIMESTAMP_NE));
    cat->InsertOperator(
        MakeOp(kInvalidOid, "<", kTimestampOid, kTimestampOid, kBoolOid, F_TIMESTAMP_LT));
    cat->InsertOperator(
        MakeOp(kInvalidOid, "<=", kTimestampOid, kTimestampOid, kBoolOid, F_TIMESTAMP_LE));
    cat->InsertOperator(
        MakeOp(kInvalidOid, ">", kTimestampOid, kTimestampOid, kBoolOid, F_TIMESTAMP_GT));
    cat->InsertOperator(
        MakeOp(kInvalidOid, ">=", kTimestampOid, kTimestampOid, kBoolOid, F_TIMESTAMP_GE));

    // --- Functions (pg_proc) ---
    //
    // Input functions take kUnknownOid (cstring stand-in) and return their
    // type; output functions are the inverse. SQL aliases that would share a
    // PostgreSQL OID with another entry (substr/substring, ceiling/ceil,
    // extract/date_part, to_date/to_timestamp) are given kInvalidOid so the
    // catalog assigns a unique OID rather than creating a duplicate.

    // Input/output functions.
    cat->InsertProc(MakeProc(1242, "boolin", kBoolOid, {kUnknownOid}));
    cat->InsertProc(MakeProc(1243, "boolout", kUnknownOid, {kBoolOid}));
    cat->InsertProc(MakeProc(42, "int4in", kInt4Oid, {kUnknownOid}));
    cat->InsertProc(MakeProc(46, "int4out", kUnknownOid, {kInt4Oid}));
    cat->InsertProc(MakeProc(48, "int8in", kInt8Oid, {kUnknownOid}));
    cat->InsertProc(MakeProc(47, "int8out", kUnknownOid, {kInt8Oid}));
    cat->InsertProc(MakeProc(34, "int2in", kInt2Oid, {kUnknownOid}));
    cat->InsertProc(MakeProc(38, "int2out", kUnknownOid, {kInt2Oid}));
    cat->InsertProc(MakeProc(200, "float4in", kFloat4Oid, {kUnknownOid}));
    cat->InsertProc(MakeProc(201, "float4out", kUnknownOid, {kFloat4Oid}));
    cat->InsertProc(MakeProc(202, "float8in", kFloat8Oid, {kUnknownOid}));
    cat->InsertProc(MakeProc(204, "float8out", kUnknownOid, {kFloat8Oid}));
    cat->InsertProc(MakeProc(40, "textin", kTextOid, {kUnknownOid}));
    cat->InsertProc(MakeProc(41, "textout", kUnknownOid, {kTextOid}));

    // String functions.
    cat->InsertProc(MakeProc(870, "lower", kTextOid, {kTextOid}));
    cat->InsertProc(MakeProc(871, "upper", kTextOid, {kTextOid}));
    cat->InsertProc(MakeProc(1311, "length", kInt4Oid, {kTextOid}));
    cat->InsertProc(MakeProc(936, "substring", kTextOid, {kTextOid, kInt4Oid, kInt4Oid}));
    cat->InsertProc(MakeProc(kInvalidOid, "substring", kTextOid, {kTextOid, kInt4Oid}));
    cat->InsertProc(MakeProc(kInvalidOid, "substr", kTextOid, {kTextOid, kInt4Oid, kInt4Oid}));
    cat->InsertProc(MakeProc(kInvalidOid, "substr", kTextOid, {kTextOid, kInt4Oid}));
    cat->InsertProc(MakeProc(884, "btrim", kTextOid, {kTextOid}));
    cat->InsertProc(MakeProc(872, "ltrim", kTextOid, {kTextOid}));
    cat->InsertProc(MakeProc(873, "rtrim", kTextOid, {kTextOid}));
    cat->InsertProc(MakeProc(3665, "concat", kTextOid, {kTextOid, kTextOid}));
    cat->InsertProc(MakeProc(2087, "replace", kTextOid, {kTextOid, kTextOid, kTextOid}));
    cat->InsertProc(MakeProc(849, "position", kInt4Oid, {kTextOid, kTextOid}));
    cat->InsertProc(MakeProc(3724, "split_part", kTextOid, {kTextOid, kTextOid, kInt4Oid}));
    cat->InsertProc(MakeProc(2284, "regexp_replace", kTextOid, {kTextOid, kTextOid, kTextOid}));
    cat->InsertProc(MakeProc(kInvalidOid, "regexp_replace", kTextOid,
                             {kTextOid, kTextOid, kTextOid, kTextOid}));

    // Math functions.
    cat->InsertProc(MakeProc(1398, "abs", kInt4Oid, {kInt4Oid}));
    cat->InsertProc(MakeProc(1700, "round", kFloat8Oid, {kFloat8Oid}));
    cat->InsertProc(MakeProc(2308, "ceil", kFloat8Oid, {kFloat8Oid}));
    cat->InsertProc(MakeProc(kInvalidOid, "ceiling", kFloat8Oid, {kFloat8Oid}));
    cat->InsertProc(MakeProc(2311, "floor", kFloat8Oid, {kFloat8Oid}));
    cat->InsertProc(MakeProc(1340, "sqrt", kFloat8Oid, {kFloat8Oid}));
    cat->InsertProc(MakeProc(1368, "power", kFloat8Oid, {kFloat8Oid, kFloat8Oid}));
    cat->InsertProc(MakeProc(941, "mod", kInt4Oid, {kInt4Oid, kInt4Oid}));

    // Date/time functions.
    cat->InsertProc(MakeProc(2020, "date_trunc", kTimestampOid, {kTextOid, kTimestampOid}));
    cat->InsertProc(MakeProc(2021, "date_part", kFloat8Oid, {kTextOid, kTimestampOid}));
    cat->InsertProc(MakeProc(kInvalidOid, "extract", kFloat8Oid, {kTextOid, kTimestampOid}));
    cat->InsertProc(MakeProc(1299, "now", kTimestamptzOid, {}));

    // Formatting functions.
    cat->InsertProc(MakeProc(1776, "to_char", kTextOid, {kTimestampOid, kTextOid}));
    cat->InsertProc(MakeProc(1764, "to_number", kNumericOid, {kTextOid, kTextOid}));
    cat->InsertProc(MakeProc(1159, "to_timestamp", kTimestamptzOid, {kTextOid, kTextOid}));
    cat->InsertProc(MakeProc(kInvalidOid, "to_date", kDateOid, {kTextOid, kTextOid}));

    // SQL-standard expression functions (modeled with int4 arguments).
    cat->InsertProc(MakeProc(kInvalidOid, "coalesce", kInt4Oid, {kInt4Oid, kInt4Oid}));
    cat->InsertProc(MakeProc(kInvalidOid, "nullif", kInt4Oid, {kInt4Oid, kInt4Oid}));
    cat->InsertProc(MakeProc(kInvalidOid, "greatest", kInt4Oid, {kInt4Oid, kInt4Oid}));
    cat->InsertProc(MakeProc(kInvalidOid, "least", kInt4Oid, {kInt4Oid, kInt4Oid}));

    // Aggregate pg_proc entries (prokind = kAggregate). These are referenced
    // by the pg_aggregate rows below via aggfnoid.
    //
    // PostgreSQL declares count/min/max with proargtypes = {anyelement} and
    // prorettype = anyelement (for min/max) or int8 (for count). Since MyToyDB
    // does not model polymorphic types, we add one entry per concrete type.
    // Return types follow PostgreSQL:
    //   count(any) → int8
    //   sum(int2/int4) → int8, sum(int8) → numeric, sum(float8) → float8
    //   avg(int2/int4/int8) → numeric, avg(float8) → float8
    //   min/max(any) → same type as input
    cat->InsertProc(MakeProc(2147, "count", kInt8Oid, {kInt4Oid}, ProKind::kAggregate));
    cat->InsertProc(MakeProc(kInvalidOid, "count", kInt8Oid, {kInt8Oid}, ProKind::kAggregate));
    cat->InsertProc(MakeProc(kInvalidOid, "count", kInt8Oid, {kTextOid}, ProKind::kAggregate));
    cat->InsertProc(MakeProc(kInvalidOid, "count", kInt8Oid, {kFloat8Oid}, ProKind::kAggregate));
    cat->InsertProc(MakeProc(kInvalidOid, "count", kInt8Oid, {kDateOid}, ProKind::kAggregate));
    cat->InsertProc(MakeProc(kInvalidOid, "count", kInt8Oid, {kTimestampOid}, ProKind::kAggregate));

    cat->InsertProc(MakeProc(2108, "sum", kInt8Oid, {kInt4Oid}, ProKind::kAggregate));
    Oid sum_int8_oid =
        cat->InsertProc(MakeProc(kInvalidOid, "sum", kNumericOid, {kInt8Oid}, ProKind::kAggregate));
    Oid sum_float8_oid = cat->InsertProc(
        MakeProc(kInvalidOid, "sum", kFloat8Oid, {kFloat8Oid}, ProKind::kAggregate));

    cat->InsertProc(MakeProc(2131, "min", kInt4Oid, {kInt4Oid}, ProKind::kAggregate));
    Oid min_int8_oid =
        cat->InsertProc(MakeProc(kInvalidOid, "min", kInt8Oid, {kInt8Oid}, ProKind::kAggregate));
    Oid min_float8_oid = cat->InsertProc(
        MakeProc(kInvalidOid, "min", kFloat8Oid, {kFloat8Oid}, ProKind::kAggregate));
    Oid min_date_oid =
        cat->InsertProc(MakeProc(kInvalidOid, "min", kDateOid, {kDateOid}, ProKind::kAggregate));
    Oid min_text_oid =
        cat->InsertProc(MakeProc(kInvalidOid, "min", kTextOid, {kTextOid}, ProKind::kAggregate));

    cat->InsertProc(MakeProc(2116, "max", kInt4Oid, {kInt4Oid}, ProKind::kAggregate));
    Oid max_int8_oid =
        cat->InsertProc(MakeProc(kInvalidOid, "max", kInt8Oid, {kInt8Oid}, ProKind::kAggregate));
    Oid max_float8_oid = cat->InsertProc(
        MakeProc(kInvalidOid, "max", kFloat8Oid, {kFloat8Oid}, ProKind::kAggregate));
    Oid max_date_oid =
        cat->InsertProc(MakeProc(kInvalidOid, "max", kDateOid, {kDateOid}, ProKind::kAggregate));
    Oid max_text_oid =
        cat->InsertProc(MakeProc(kInvalidOid, "max", kTextOid, {kTextOid}, ProKind::kAggregate));

    cat->InsertProc(MakeProc(2107, "avg", kNumericOid, {kInt4Oid}, ProKind::kAggregate));
    Oid avg_int8_oid =
        cat->InsertProc(MakeProc(kInvalidOid, "avg", kNumericOid, {kInt8Oid}, ProKind::kAggregate));
    Oid avg_float8_oid = cat->InsertProc(
        MakeProc(kInvalidOid, "avg", kFloat8Oid, {kFloat8Oid}, ProKind::kAggregate));

    // --- Aggregates (pg_aggregate) ---
    //
    // aggtransfn references PostgreSQL transition functions (int8inc, int4_sum,
    // int8_sum, int4smaller, int4larger, int4_avg_accum) whose pg_proc entries
    // are not created in this bootstrap; their OIDs are left as kInvalidOid.
    // No aggregate here has a separate final function.
    cat->InsertAggregate(MakeAgg(2147, kInvalidOid, kInvalidOid, kInt8Oid, "0"));  // count(int4)
    cat->InsertAggregate(MakeAgg(2108, kInvalidOid, kInvalidOid, kInt8Oid));       // sum(int4)
    cat->InsertAggregate(
        MakeAgg(sum_int8_oid, kInvalidOid, kInvalidOid, kNumericOid));  // sum(int8)
    cat->InsertAggregate(
        MakeAgg(sum_float8_oid, kInvalidOid, kInvalidOid, kFloat8Oid));       // sum(float8)
    cat->InsertAggregate(MakeAgg(2131, kInvalidOid, kInvalidOid, kInt4Oid));  // min(int4)
    cat->InsertAggregate(MakeAgg(min_int8_oid, kInvalidOid, kInvalidOid, kInt8Oid));
    cat->InsertAggregate(MakeAgg(min_float8_oid, kInvalidOid, kInvalidOid, kFloat8Oid));
    cat->InsertAggregate(MakeAgg(min_date_oid, kInvalidOid, kInvalidOid, kDateOid));
    cat->InsertAggregate(MakeAgg(min_text_oid, kInvalidOid, kInvalidOid, kTextOid));
    cat->InsertAggregate(MakeAgg(2116, kInvalidOid, kInvalidOid, kInt4Oid));  // max(int4)
    cat->InsertAggregate(MakeAgg(max_int8_oid, kInvalidOid, kInvalidOid, kInt8Oid));
    cat->InsertAggregate(MakeAgg(max_float8_oid, kInvalidOid, kInvalidOid, kFloat8Oid));
    cat->InsertAggregate(MakeAgg(max_date_oid, kInvalidOid, kInvalidOid, kDateOid));
    cat->InsertAggregate(MakeAgg(max_text_oid, kInvalidOid, kInvalidOid, kTextOid));
    cat->InsertAggregate(MakeAgg(2107, kInvalidOid, kInvalidOid, kInt8Oid));  // avg(int4)
    cat->InsertAggregate(MakeAgg(avg_int8_oid, kInvalidOid, kInvalidOid, kNumericOid));
    cat->InsertAggregate(MakeAgg(avg_float8_oid, kInvalidOid, kInvalidOid, kFloat8Oid));

    // --- Casts (pg_cast) ---
    //
    // Numeric widening casts are implicit; narrowing casts are assignment-
    // context. castfunc references PostgreSQL cast functions (int42, int48, ...)
    // whose pg_proc entries are not created here, so castfunc is kInvalidOid.
    // text/varchar are binary-compatible; int4->bool is explicit.
    cat->InsertCast(
        MakeCast(kInt2Oid, kInt4Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt2Oid, kInt8Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt4Oid, kInt8Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt4Oid, kInt2Oid, kInvalidOid, CastContext::kAssignment, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt8Oid, kInt4Oid, kInvalidOid, CastContext::kAssignment, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt8Oid, kInt2Oid, kInvalidOid, CastContext::kAssignment, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt2Oid, kFloat4Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt2Oid, kFloat8Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt4Oid, kFloat4Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt4Oid, kFloat8Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt8Oid, kFloat4Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kInt8Oid, kFloat8Oid, kInvalidOid, CastContext::kImplicit, CastMethod::kFunction));
    cat->InsertCast(MakeCast(kFloat4Oid, kFloat8Oid, kInvalidOid, CastContext::kImplicit,
                             CastMethod::kFunction));
    cat->InsertCast(MakeCast(kFloat8Oid, kFloat4Oid, kInvalidOid, CastContext::kAssignment,
                             CastMethod::kFunction));
    cat->InsertCast(MakeCast(kFloat4Oid, kInt2Oid, kInvalidOid, CastContext::kAssignment,
                             CastMethod::kFunction));
    cat->InsertCast(MakeCast(kFloat4Oid, kInt4Oid, kInvalidOid, CastContext::kAssignment,
                             CastMethod::kFunction));
    cat->InsertCast(MakeCast(kFloat4Oid, kInt8Oid, kInvalidOid, CastContext::kAssignment,
                             CastMethod::kFunction));
    cat->InsertCast(MakeCast(kFloat8Oid, kInt2Oid, kInvalidOid, CastContext::kAssignment,
                             CastMethod::kFunction));
    cat->InsertCast(MakeCast(kFloat8Oid, kInt4Oid, kInvalidOid, CastContext::kAssignment,
                             CastMethod::kFunction));
    cat->InsertCast(MakeCast(kFloat8Oid, kInt8Oid, kInvalidOid, CastContext::kAssignment,
                             CastMethod::kFunction));
    cat->InsertCast(
        MakeCast(kTextOid, kVarcharOid, kInvalidOid, CastContext::kImplicit, CastMethod::kBinary));
    cat->InsertCast(
        MakeCast(kVarcharOid, kTextOid, kInvalidOid, CastContext::kImplicit, CastMethod::kBinary));
    cat->InsertCast(
        MakeCast(kInt4Oid, kBoolOid, kInvalidOid, CastContext::kExplicit, CastMethod::kFunction));

    // --- Collations (pg_collation) ---
    cat->InsertCollation(MakeCollation(kDefaultCollationOid, "default", CollProvider::kDefault));
    cat->InsertCollation(MakeCollation(kC_COLLATION_OID, "C", CollProvider::kLibc));
    cat->InsertCollation(MakeCollation(kPOSIX_COLLATION_OID, "POSIX", CollProvider::kLibc));
}

}  // namespace mytoydb::catalog
