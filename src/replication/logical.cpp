// logical.cpp — Logical decoding context.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/ logical.c.
// The actual decoding loop is stubbed.
#include "pgcpp/replication/logical.hpp"

#include <string>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/replication/replutil.hpp"
#include "pgcpp/replication/slot.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace mytoydb::replication {

using mytoydb::error::LogLevel;

namespace {

LogicalDecodingContext& ActiveCtx() {
    static LogicalDecodingContext c;
    return c;
}

}  // namespace

LogicalDecodingContext CreateInitDecodingContext(const std::string& plugin_name,
                                                 const std::string& slot_name,
                                                 const std::string& database,
                                                 LogicalDecodingOptions options) {
    LogicalDecodingContext ctx;
    std::string effective_plugin = plugin_name.empty() ? DefaultOutputPluginName() : plugin_name;
    if (slot_name.empty()) {
        ereport(LogLevel::kError, "CreateInitDecodingContext: slot_name is required");
        return ctx;
    }
    // Create a logical slot with the requested plugin.
    bool ok = false;
    PG_TRY() {
        ok = ReplicationSlotCreate(slot_name, SlotType::kLogical, SlotPersistence::kPersistent,
                                   effective_plugin, database);
    }
    PG_CATCH() {
        ok = false;
    }
    PG_END_TRY();
    if (!ok) {
        return ctx;
    }
    const ReplicationSlot* s = ReplicationSlotLookup(slot_name);
    if (s == nullptr) {
        ereport(LogLevel::kError,
                "CreateInitDecodingContext: slot \"" + slot_name + "\" vanished after create");
        return ctx;
    }
    ctx.plugin_name = effective_plugin;
    ctx.slot_name = slot_name;
    ctx.options = options;
    ctx.start_lsn = s->restart_lsn;
    ctx.prepared = true;
    ActiveCtx() = ctx;
    return ctx;
}

LogicalDecodingContext CreateDecodingContext(const std::string& slot_name,
                                             LogicalDecodingOptions options) {
    LogicalDecodingContext ctx;
    const ReplicationSlot* s = ReplicationSlotLookup(slot_name);
    if (s == nullptr) {
        ereport(LogLevel::kError,
                "CreateDecodingContext: slot \"" + slot_name + "\" does not exist");
        return ctx;
    }
    if (s->type != SlotType::kLogical) {
        ereport(LogLevel::kError,
                "CreateDecodingContext: slot \"" + slot_name + "\" is not logical");
        return ctx;
    }
    ctx.plugin_name = s->plugin;
    ctx.slot_name = slot_name;
    ctx.options = options;
    ctx.start_lsn = s->confirmed_flush_lsn;
    ctx.prepared = true;
    ActiveCtx() = ctx;
    return ctx;
}

int LogicalShippingMain(LogicalDecodingContext& ctx, int max_messages) {
    if (!ctx.prepared) {
        ereport(LogLevel::kError, "LogicalShippingMain: context not prepared");
        return -1;
    }
    // Stubbed: synthesize `max_messages` BEGIN/COMMIT pairs and stop.
    int emitted = 0;
    for (int i = 0; i < max_messages; ++i) {
        DecodingEmitMessage(ctx, LogicalRepMsgType::kBegin, "BEGIN tx=" + std::to_string(i));
        DecodingEmitMessage(ctx, LogicalRepMsgType::kCommit, "COMMIT tx=" + std::to_string(i));
        emitted += 2;
    }
    ctx.end_lsn = transaction::GetXLogInsertRecPtr();
    return emitted;
}

void LogicalDecodingReset() {
    ActiveCtx() = LogicalDecodingContext{};
}

LogicalDecodingContext* GetLogicalDecodingContext() {
    return &ActiveCtx();
}

void DecodingEmitMessage(LogicalDecodingContext& ctx, LogicalRepMsgType type,
                         const std::string& payload) {
    std::string msg = std::string(LogicalRepMsgTypeName(type)) + ": " + payload;
    ctx.messages.push_back(msg);
    ctx.output_buffer += msg;
    ctx.output_buffer.push_back('\n');
}

}  // namespace mytoydb::replication
