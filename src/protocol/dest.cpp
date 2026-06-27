// dest.cpp — DestReceiver: polymorphic destination for query result tuples.
//
// Converted from PostgreSQL 15's src/backend/tcop/dest.c.
//
// Implements the concrete DestReceiver subclasses:
//   NoneReceiver        — discards all rows
//   DebugReceiver       — prints tuples to stdout
//   RemoteReceiver      — sends RowDescription + DataRow via OutputSink
//   TuplestoreReceiver  — collects slots into a vector
//   IntoRelReceiver     — inserts tuples into a relation
#include "mytoydb/protocol/dest.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "mytoydb/access/heapam.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/executor/estate.hpp"
#include "mytoydb/executor/tupletable.hpp"
#include "mytoydb/transaction/heap_tuple.hpp"
#include "mytoydb/types/builtins.hpp"
#include "mytoydb/types/datetime.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::protocol {

using mytoydb::access::heap_form_tuple;
using mytoydb::access::heap_freetuple;
using mytoydb::access::heap_insert;
using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationOpen;
using mytoydb::access::TupleDesc;
using mytoydb::catalog::Oid;
using mytoydb::executor::QueryDesc;
using mytoydb::executor::TupleTableSlot;
using mytoydb::transaction::HeapTuple;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetFloat4;
using mytoydb::types::DatumGetFloat8;
using mytoydb::types::DatumGetInt16;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::kBoolOid;
using mytoydb::types::kDateOid;
using mytoydb::types::kFloat4Oid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::kTimestampOid;
using mytoydb::types::kVarcharOid;
using mytoydb::types::TextDatumToString;

// Encode a Datum value to its text representation based on the type OID.
// This mirrors Backend's EncodeValue in postgres.cpp; kept separate to avoid
// coupling the DestReceiver layer to the Backend class.
std::string EncodeDatumAsText(Datum value, Oid type_oid) {
    switch (type_oid) {
        case kBoolOid: {
            char* s = mytoydb::types::bool_out(value);
            return std::string(s);
        }
        case kInt2Oid:
            return std::to_string(DatumGetInt16(value));
        case kInt4Oid: {
            char* s = mytoydb::types::int4_out(value);
            return std::string(s);
        }
        case kInt8Oid: {
            char* s = mytoydb::types::int8_out(value);
            return std::string(s);
        }
        case kFloat4Oid:
            return std::to_string(DatumGetFloat4(value));
        case kFloat8Oid: {
            char* s = mytoydb::types::float8_out(value);
            return std::string(s);
        }
        case kTextOid:
        case kVarcharOid:
            return TextDatumToString(value);
        case kDateOid: {
            char* s = mytoydb::types::date_out(value);
            return std::string(s);
        }
        case kTimestampOid: {
            char* s = mytoydb::types::timestamp_out(value);
            return std::string(s);
        }
        default:
            return std::to_string(static_cast<int64_t>(value));
    }
}

namespace {

// ---------------------------------------------------------------------------
// NoneReceiver — discards all rows
// ---------------------------------------------------------------------------
class NoneReceiver : public DestReceiver {
public:
    NoneReceiver() { mydest = CommandDest::kNone; }
    bool receiveSlot(TupleTableSlot* /*slot*/, QueryDesc* /*query_desc*/) override { return true; }
};

// ---------------------------------------------------------------------------
// DebugReceiver — prints tuples to stdout (simplified)
// ---------------------------------------------------------------------------
class DebugReceiver : public DestReceiver {
public:
    DebugReceiver() { mydest = CommandDest::kDebug; }

    void rStartup(QueryDesc* /*query_desc*/, int /*operation*/, TupleDesc tupdesc) override {
        tupdesc_ = tupdesc;
        if (tupdesc_ != nullptr) {
            std::printf("--- DebugReceiver: %d columns ---\n", tupdesc_->natts);
            for (int i = 0; i < tupdesc_->natts; ++i) {
                std::printf("  col %d: %s\n", i + 1, tupdesc_->attrs[i].attname.c_str());
            }
        }
    }

    bool receiveSlot(TupleTableSlot* slot, QueryDesc* /*query_desc*/) override {
        if (slot == nullptr || tupdesc_ == nullptr)
            return true;
        std::printf("row:");
        for (int i = 0; i < tupdesc_->natts; ++i) {
            if (slot->tts_isnull != nullptr && slot->tts_isnull[i]) {
                std::printf(" (null)");
            } else {
                std::printf(
                    " %s",
                    EncodeDatumAsText(slot->tts_values[i], tupdesc_->attrs[i].atttypid).c_str());
            }
        }
        std::printf("\n");
        return true;
    }

private:
    TupleDesc tupdesc_ = nullptr;
};

// ---------------------------------------------------------------------------
// RemoteReceiver — sends RowDescription + DataRow via OutputSink
// ---------------------------------------------------------------------------
class RemoteReceiver : public DestReceiver {
public:
    RemoteReceiver(OutputSink* sink, bool send_row_description)
        : sink_(sink), send_row_description_(send_row_description) {
        mydest = send_row_description ? CommandDest::kRemote : CommandDest::kRemoteExecute;
    }

    void rStartup(QueryDesc* /*query_desc*/, int /*operation*/, TupleDesc tupdesc) override {
        tupdesc_ = tupdesc;
        if (send_row_description_ && tupdesc_ != nullptr) {
            std::vector<RowDescriptionField> fields;
            for (int i = 0; i < tupdesc_->natts; ++i) {
                const auto& attr = tupdesc_->attrs[i];
                RowDescriptionField f;
                f.name = attr.attname;
                f.table_oid = attr.attrelid;
                f.column_attnum = attr.attnum;
                f.type_oid = attr.atttypid;
                f.type_size = attr.attlen;
                f.type_mod = attr.atttypmod;
                f.format = 0;  // text
                fields.push_back(f);
            }
            sink_->SendMessage(BuildRowDescription(fields));
        }
    }

    bool receiveSlot(TupleTableSlot* slot, QueryDesc* /*query_desc*/) override {
        if (slot == nullptr || tupdesc_ == nullptr)
            return true;
        std::vector<std::string> values;
        std::vector<bool> isnull;
        int natts = tupdesc_->natts;
        values.reserve(static_cast<std::size_t>(natts));
        isnull.reserve(static_cast<std::size_t>(natts));
        for (int i = 0; i < natts; ++i) {
            bool null = (slot->tts_isnull != nullptr) && slot->tts_isnull[i];
            isnull.push_back(null);
            if (null) {
                values.emplace_back();
            } else {
                values.push_back(
                    EncodeDatumAsText(slot->tts_values[i], tupdesc_->attrs[i].atttypid));
            }
        }
        sink_->SendMessage(BuildDataRow(values, isnull));
        return true;
    }

private:
    OutputSink* sink_;
    bool send_row_description_;
    TupleDesc tupdesc_ = nullptr;
};

// ---------------------------------------------------------------------------
// TuplestoreReceiver — collects slots into an in-memory vector
// ---------------------------------------------------------------------------
class TuplestoreReceiver : public DestReceiver {
public:
    TuplestoreReceiver() { mydest = CommandDest::kTuplestore; }

    ~TuplestoreReceiver() override { FreeSlots(); }

    void rStartup(QueryDesc* /*query_desc*/, int /*operation*/, TupleDesc tupdesc) override {
        tupdesc_ = tupdesc;
        FreeSlots();
    }

    bool receiveSlot(TupleTableSlot* slot, QueryDesc* /*query_desc*/) override {
        if (slot == nullptr)
            return true;
        // Deep-copy the slot so the collected data survives after the executor
        // reuses/frees the original slot.
        TupleTableSlot* copy = TupleTableSlot::Make(tupdesc_);
        copy->StoreVirtual(slot->tts_values, slot->tts_isnull);
        slots_.push_back(copy);
        return true;
    }

    void rDestroy() override {
        FreeSlots();
        tupdesc_ = nullptr;
    }

    std::vector<TupleTableSlot*> slots() const { return slots_; }

private:
    void FreeSlots() {
        for (TupleTableSlot* s : slots_) {
            delete s;
        }
        slots_.clear();
    }

    TupleDesc tupdesc_ = nullptr;
    std::vector<TupleTableSlot*> slots_;
};

// ---------------------------------------------------------------------------
// IntoRelReceiver — inserts each tuple into a relation
// ---------------------------------------------------------------------------
class IntoRelReceiver : public DestReceiver {
public:
    explicit IntoRelReceiver(Oid relid) : relid_(relid) { mydest = CommandDest::kIntoRel; }

    ~IntoRelReceiver() override {
        if (rel_ != nullptr) {
            RelationClose(rel_);
        }
    }

    void rStartup(QueryDesc* /*query_desc*/, int /*operation*/, TupleDesc tupdesc) override {
        tupdesc_ = tupdesc;
        rel_ = RelationOpen(relid_);
        if (rel_ == nullptr) {
            ereport(mytoydb::error::LogLevel::kError,
                    "IntoRelReceiver: relation " + std::to_string(relid_) + " not found");
        }
    }

    bool receiveSlot(TupleTableSlot* slot, QueryDesc* /*query_desc*/) override {
        if (slot == nullptr || rel_ == nullptr || tupdesc_ == nullptr)
            return true;
        HeapTuple tup = heap_form_tuple(tupdesc_, slot->tts_values, slot->tts_isnull);
        heap_insert(rel_, tup);
        heap_freetuple(tup);
        return true;
    }

    void rShutdown(QueryDesc* /*query_desc*/) override {
        if (rel_ != nullptr) {
            RelationClose(rel_);
            rel_ = nullptr;
        }
    }

    void rDestroy() override {
        if (rel_ != nullptr) {
            RelationClose(rel_);
            rel_ = nullptr;
        }
    }

private:
    Oid relid_;
    TupleDesc tupdesc_ = nullptr;
    Relation rel_ = nullptr;
};

}  // namespace

// --- Factory and constructor functions ---

DestReceiver* CreateDestReceiver(CommandDest dest, OutputSink* sink) {
    switch (dest) {
        case CommandDest::kNone:
            return CreateNoneReceiver();
        case CommandDest::kDebug:
            return CreateDebugReceiver();
        case CommandDest::kRemote:
            return CreateRemoteReceiver(sink, /*send_row_description=*/true);
        case CommandDest::kRemoteExecute:
            return CreateRemoteReceiver(sink, /*send_row_description=*/false);
        case CommandDest::kTuplestore:
        case CommandDest::kSQLFunction:
            return CreateTuplestoreReceiver();
        case CommandDest::kIntoRel:
            // IntoRel needs a relid; fall back to None for the generic factory.
            return CreateNoneReceiver();
    }
    return CreateNoneReceiver();
}

DestReceiver* CreateNoneReceiver() {
    return new NoneReceiver();
}

DestReceiver* CreateDebugReceiver() {
    return new DebugReceiver();
}

DestReceiver* CreateRemoteReceiver(OutputSink* sink, bool send_row_description) {
    return new RemoteReceiver(sink, send_row_description);
}

DestReceiver* CreateTuplestoreReceiver() {
    return new TuplestoreReceiver();
}

DestReceiver* CreateIntoRelReceiver(Oid relid) {
    return new IntoRelReceiver(relid);
}

std::vector<TupleTableSlot*> GetTuplestoreSlots(DestReceiver* receiver) {
    if (receiver == nullptr || receiver->mydest != CommandDest::kTuplestore)
        return {};
    auto* ts = dynamic_cast<TuplestoreReceiver*>(receiver);
    if (ts == nullptr)
        return {};
    return ts->slots();
}

}  // namespace mytoydb::protocol
