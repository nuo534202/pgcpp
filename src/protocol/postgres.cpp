// postgres.cpp — Frontend protocol handler (simple and extended query).
//
// Converted from PostgreSQL 15's src/backend/tcop/postgres.c.
//
// Orchestrates the full query processing pipeline:
//   raw_parser() -> parse_analyze() -> planner() -> ExecutorStart/Run/Finish/End
//
// Errors from ereport(ERROR) are caught with PG_TRY/PG_CATCH and converted
// to ErrorResponse messages sent to the client.
#include "protocol/postgres.hpp"

#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_main.hpp"
#include "executor/exec_utils.hpp"
#include "executor/node_exec.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "optimizer/planner.hpp"
#include "parser/analyze.hpp"
#include "parser/parse_type.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "parser/primnodes.hpp"
#include "protocol/utility.hpp"
#include "rewrite/rewrite_handler.hpp"
#include "server/postmaster.hpp"
#include "transaction/xact.hpp"
#include "types/builtins.hpp"
#include "types/datetime.hpp"
#include "types/datum.hpp"
#include "utils/cache/plancache.hpp"

namespace pgcpp::protocol {

using pgcpp::access::TupleDesc;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::executor::BuildTupleDescFromTargetList;
using pgcpp::executor::ClearExecParams;
using pgcpp::executor::ExecutorEnd;
using pgcpp::executor::ExecutorFinish;
using pgcpp::executor::ExecutorRun;
using pgcpp::executor::ExecutorStart;
using pgcpp::executor::Plan;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::SetExecParams;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::palloc;
using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::planner;
using pgcpp::parser::CmdType;
using pgcpp::parser::Node;
using pgcpp::parser::Query;
using pgcpp::parser::RangeVar;
using pgcpp::parser::RawStmt;
using pgcpp::parser::RelabelType;
using pgcpp::parser::TargetEntry;
using pgcpp::transaction::AbortCurrentTransaction;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::CommandCounterIncrement;
using pgcpp::transaction::CommitTransactionCommand;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::IsAbortedTransactionBlock;
using pgcpp::transaction::IsTransactionBlock;
using pgcpp::transaction::StartTransactionCommand;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetFloat4;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt16;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::kBoolOid;
using pgcpp::types::kDateOid;
using pgcpp::types::kFloat4Oid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt2Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;
using pgcpp::types::kVarcharOid;
using pgcpp::types::TextDatumToString;
using pgcpp::utils::CachedPlan;
using pgcpp::utils::CachedPlanSource;

namespace {

// Convert a text parameter value to a Datum based on the parameter type OID.
Datum ConvertParamToDatum(const std::string& text_value, Oid type_oid) {
    switch (type_oid) {
        case kBoolOid:
            return pgcpp::types::bool_in(text_value.c_str());
        case kInt2Oid:
        case kInt4Oid:
            return pgcpp::types::int4_in(text_value.c_str());
        case kInt8Oid:
            return pgcpp::types::int8_in(text_value.c_str());
        case kFloat4Oid:
        case kFloat8Oid:
            return pgcpp::types::float8_in(text_value.c_str());
        case kTextOid:
        case kVarcharOid:
            return pgcpp::types::text_in(text_value.c_str());
        default:
            // Default: treat as int4.
            return pgcpp::types::int4_in(text_value.c_str());
    }
}

// Encode a Datum value to its text representation based on the type OID.
// Returns the encoded string. For null values, the caller should check isnull
// before calling this function.
std::string EncodeValue(Datum value, Oid type_oid) {
    switch (type_oid) {
        case kBoolOid: {
            char* s = pgcpp::types::bool_out(value);
            std::string result(s);
            return result;
        }
        case kInt2Oid:
            // int2 is stored as int16; reuse int4_out (same format).
            return std::to_string(DatumGetInt16(value));
        case kInt4Oid: {
            char* s = pgcpp::types::int4_out(value);
            return std::string(s);
        }
        case kInt8Oid: {
            char* s = pgcpp::types::int8_out(value);
            return std::string(s);
        }
        case kFloat4Oid: {
            // float4 is stored as float; format via float8_out for simplicity.
            return std::to_string(DatumGetFloat4(value));
        }
        case kFloat8Oid: {
            char* s = pgcpp::types::float8_out(value);
            return std::string(s);
        }
        case kTextOid:
        case kVarcharOid: {
            return TextDatumToString(value);
        }
        case kDateOid: {
            char* s = pgcpp::types::date_out(value);
            return std::string(s);
        }
        case kTimestampOid: {
            char* s = pgcpp::types::timestamp_out(value);
            return std::string(s);
        }
        default:
            // Unknown type: encode as a hex representation of the Datum.
            return std::to_string(static_cast<int64_t>(value));
    }
}

// Build RowDescription fields from a Query's target list.
std::vector<RowDescriptionField> BuildRowDescriptionFields(Query* query) {
    std::vector<RowDescriptionField> fields;
    // Build a TupleDesc from the target list to get type metadata.
    std::vector<TargetEntry*> targetlist;
    for (Node* node : query->target_list) {
        targetlist.push_back(static_cast<TargetEntry*>(node));
    }
    TupleDesc tupdesc = BuildTupleDescFromTargetList(targetlist);

    for (std::size_t i = 0; i < targetlist.size(); ++i) {
        TargetEntry* te = targetlist[i];
        if (te->resjunk)
            continue;

        RowDescriptionField f;
        f.name = te->resname.empty() ? "?column?" : te->resname;
        f.table_oid = 0;
        f.column_attnum = 0;
        if (i < tupdesc->attrs.size()) {
            f.type_oid = tupdesc->attrs[i].atttypid;
            f.type_size = tupdesc->attrs[i].attlen;
            f.type_mod = tupdesc->attrs[i].atttypmod;
        } else {
            f.type_oid = kInt4Oid;
            f.type_size = 4;
            f.type_mod = -1;
        }
        f.format = 0;  // text format
        fields.push_back(f);
    }
    return fields;
}

// Determine the type OID of a TargetEntry's expression.
Oid GetExprTypeOid(Node* expr) {
    if (expr == nullptr)
        return kInt4Oid;
    switch (expr->GetTag()) {
        case pgcpp::nodes::NodeTag::kVar: {
            auto* v = static_cast<pgcpp::parser::Var*>(expr);
            return v->vartype;
        }
        case pgcpp::nodes::NodeTag::kConst: {
            auto* c = static_cast<pgcpp::parser::Const*>(expr);
            return c->consttype;
        }
        case pgcpp::nodes::NodeTag::kAggref: {
            auto* a = static_cast<pgcpp::parser::Aggref*>(expr);
            return a->aggtype;
        }
        case pgcpp::nodes::NodeTag::kOpExpr: {
            auto* op = static_cast<pgcpp::parser::OpExpr*>(expr);
            return op->opresulttype;
        }
        case pgcpp::nodes::NodeTag::kFuncExpr: {
            auto* fn = static_cast<pgcpp::parser::FuncExpr*>(expr);
            return fn->funcresulttype;
        }
        default:
            return kInt4Oid;
    }
}

}  // namespace

// --- Backend ---

PreparedStatement::~PreparedStatement() {
    for (auto* ps : plan_sources) {
        delete ps;
    }
}

Backend::Backend(OutputSink* sink) : sink_(sink) {}

Backend::~Backend() {
    // Clean up prepared statements and portals.
    for (auto& [name, stmt] : prepared_statements_) {
        // Queries are palloc'd; we don't free them here since they may be
        // in a memory context that's already been reset. The memory context
        // system handles cleanup.
        delete stmt;
    }
    for (auto& [name, portal] : portals_) {
        delete portal;
    }
}

void Backend::exec_simple_query(const std::string& query_string) {
    // Check for empty query.
    if (query_string.empty() || query_string.find_first_not_of(" \t\n\r;") == std::string::npos) {
        sink_->SendMessage(BuildEmptyQueryResponse());
        sink_->SendMessage(BuildReadyForQuery(GetCurrentTransactionStatus()));
        return;
    }

    // Parse the query string into raw statements.
    // NOTE: We analyze each statement individually just before executing it
    // (instead of batch-analyzing all statements up front) so that DDL changes
    // (e.g., CREATE TABLE) are visible to the parse analysis of subsequent
    // statements (e.g., INSERT, SELECT) in the same query string. This mirrors
    // PostgreSQL's behavior where each statement is fully processed before the
    // next one is analyzed.
    std::vector<RawStmt*> raw_stmts;

    PG_TRY() {
        raw_stmts = pgcpp::parser::raw_parser(query_string);
    }
    PG_CATCH() {
        pgcpp::error::ErrorData* ed = pgcpp::error::GetErrorData();
        SendError(ed ? ed->message : "parse error");
        AbortCurrentTransaction();
        sink_->SendMessage(BuildReadyForQuery(GetCurrentTransactionStatus()));
        return;
    }
    PG_END_TRY();

    // Process each raw statement: analyze it, then execute it, one at a time.
    for (size_t stmt_idx = 0; stmt_idx < raw_stmts.size(); ++stmt_idx) {
        RawStmt* raw_stmt = raw_stmts[stmt_idx];
        PG_TRY() {
            StartTransactionCommand();

            // Analyze this single statement. Analyzing one at a time ensures
            // that any catalog changes from prior statements (e.g. CREATE
            // TABLE) are visible to this statement's parse analysis.
            std::vector<RawStmt*> single_stmt{raw_stmt};
            std::vector<Query*> queries =
                pgcpp::parser::parse_analyze(single_stmt, query_string.c_str());

            if (queries.empty()) {
                CommitTransactionCommand();
                continue;
            }

            Query* query = queries.front();

            // Apply query rewrite (view expansion, RLS policies).
            query = pgcpp::rewrite::QueryRewrite(query);

            std::string tag;
            if (query->command_type == CmdType::kUtility) {
                tag = ProcessUtility(query->utility_stmt, sink_);
                if (tag.empty()) {
                    tag = "UTILITY";
                }
            } else {
                tag = ExecuteQuery(query);
            }

            CommitTransactionCommand();
            sink_->SendMessage(BuildCommandComplete(tag));
        }
        PG_CATCH() {
            pgcpp::error::ErrorData* ed = pgcpp::error::GetErrorData();
            SendError(ed ? ed->message : "execution error");
            AbortCurrentTransaction();
            // Stop processing remaining statements.
            sink_->SendMessage(BuildReadyForQuery(GetCurrentTransactionStatus()));
            return;
        }
        PG_END_TRY();
    }

    sink_->SendMessage(BuildReadyForQuery(GetCurrentTransactionStatus()));
}

std::string Backend::ExecuteQuery(Query* query, Plan* plan, bool send_row_description) {
    // Plan the query if no pre-built plan was provided (simple query path).
    if (plan == nullptr) {
        plan = planner(query);
    }

    // Create a QueryDesc.
    auto* qd = makePallocNode<QueryDesc>();
    qd->query = query;
    qd->plan = plan;

    // For SELECT, send RowDescription first (simple query protocol only).
    bool is_select = (query->command_type == CmdType::kSelect);
    if (is_select && send_row_description) {
        SendRowDescription(query);
    }

    // Execute.
    ExecutorStart(qd);

    // Extract a constant LIMIT value to apply as a fallback when the plan
    // tree does not already enforce it (e.g., no Sort/Top-N node). When a
    // Sort node with Top-N exists, it already truncates results; this loop
    // guard only matters for LIMIT without ORDER BY.
    int64_t row_limit = -1;
    if (query->limit_count != nullptr) {
        Node* lc_node = query->limit_count;
        // Unwrap RelabelType (e.g., int4->int8 binary coercion).
        if (lc_node->GetTag() == pgcpp::nodes::NodeTag::kRelabelType) {
            lc_node = static_cast<RelabelType*>(lc_node)->arg;
        }
        if (lc_node != nullptr && lc_node->GetTag() == pgcpp::nodes::NodeTag::kConst) {
            auto* lc = static_cast<pgcpp::parser::Const*>(lc_node);
            if (!lc->constisnull)
                row_limit = DatumGetInt64(lc->constvalue);
        }
    }

    int row_count = 0;
    if (is_select) {
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            // Encode the row values.
            std::vector<std::string> values;
            std::vector<bool> isnull;

            // Get the type OIDs from the target list.
            std::vector<TargetEntry*> targetlist;
            for (Node* node : query->target_list) {
                targetlist.push_back(static_cast<TargetEntry*>(node));
            }

            for (std::size_t i = 0; i < targetlist.size(); ++i) {
                TargetEntry* te = targetlist[i];
                if (te->resjunk)
                    continue;

                bool null = slot->tts_isnull[i];
                isnull.push_back(null);
                if (null) {
                    values.emplace_back("");
                } else {
                    Oid typoid = GetExprTypeOid(te->expr);
                    values.push_back(EncodeValue(slot->tts_values[i], typoid));
                }
            }
            SendDataRow(query, values, isnull);
            row_count++;
            // Apply LIMIT fallback when no Sort/Top-N node enforces it.
            if (row_limit >= 0 && row_count >= row_limit)
                break;
        }
    } else {
        // For DML, run to completion (ModifyTable returns tuples for RETURNING,
        // but for now we just count rows affected).
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            row_count++;
        }
    }

    ExecutorFinish(qd);
    ExecutorEnd(qd);

    // Build the command tag.
    std::string tag;
    switch (query->command_type) {
        case CmdType::kSelect:
            tag = "SELECT " + std::to_string(row_count);
            break;
        case CmdType::kInsert:
            tag = "INSERT 0 " + std::to_string(row_count);
            break;
        case CmdType::kUpdate:
            tag = "UPDATE " + std::to_string(row_count);
            break;
        case CmdType::kDelete:
            tag = "DELETE " + std::to_string(row_count);
            break;
        default:
            tag = "OK";
            break;
    }
    return tag;
}

void Backend::SendRowDescription(Query* query) {
    auto fields = BuildRowDescriptionFields(query);
    sink_->SendMessage(BuildRowDescription(fields));
}

void Backend::SendDataRow(Query* /*query*/, const std::vector<std::string>& values,
                          const std::vector<bool>& isnull) {
    sink_->SendMessage(BuildDataRow(values, isnull));
}

TransactionStatus Backend::GetCurrentTransactionStatus() const {
    if (IsAbortedTransactionBlock()) {
        return TransactionStatus::kInFailedTransaction;
    }
    if (IsTransactionBlock()) {
        return TransactionStatus::kInTransaction;
    }
    return TransactionStatus::kIdle;
}

void Backend::SendError(const std::string& message) {
    sink_->SendMessage(BuildErrorResponse(message));
}

// --- Extended Query Protocol ---

void Backend::HandleParse(const std::string& stmt_name, const std::string& query,
                          const std::vector<Oid>& param_types) {
    PG_TRY() {
        std::vector<RawStmt*> raw_stmts = pgcpp::parser::raw_parser(query);
        std::vector<Query*> queries = pgcpp::parser::parse_analyze(raw_stmts, query.c_str());

        // Apply query rewrite (view expansion, RLS policies) to each query.
        for (auto* q : queries) {
            pgcpp::rewrite::QueryRewrite(q);
        }

        // Replace any existing prepared statement with the same name.
        auto it = prepared_statements_.find(stmt_name);
        if (it != prepared_statements_.end()) {
            delete it->second;
            prepared_statements_.erase(it);
        }

        auto* stmt = new PreparedStatement();
        stmt->name = stmt_name;
        stmt->queries = queries;
        stmt->param_types = param_types;
        stmt->has_results = !queries.empty() && queries[0]->command_type == CmdType::kSelect;
        // Create a plan source for each query (lazy — plans are built on
        // first Execute and cached for subsequent uses).
        for (auto* q : queries) {
            stmt->plan_sources.push_back(new CachedPlanSource(q, query));
        }
        prepared_statements_[stmt_name] = stmt;

        sink_->SendMessage(BuildParseComplete());
    }
    PG_CATCH() {
        pgcpp::error::ErrorData* ed = pgcpp::error::GetErrorData();
        SendError(ed ? ed->message : "parse error");
    }
    PG_END_TRY();
}

void Backend::HandleBind(const std::string& portal_name, const std::string& stmt_name,
                         const std::vector<std::string>& param_values,
                         const std::vector<bool>& param_isnull) {
    PG_TRY() {
        PreparedStatement* stmt = FindPreparedStatement(stmt_name);
        if (stmt == nullptr) {
            ereport(pgcpp::error::LogLevel::kError,
                    "prepared statement \"" + stmt_name + "\" does not exist");
        }

        // Replace any existing portal with the same name.
        auto it = portals_.find(portal_name);
        if (it != portals_.end()) {
            delete it->second;
            portals_.erase(it);
        }

        auto* portal = new Portal();
        portal->name = portal_name;
        portal->stmt = stmt;
        portal->query_index = 0;
        portal->param_values = param_values;
        portal->param_isnull = param_isnull;
        portals_[portal_name] = portal;

        sink_->SendMessage(BuildBindComplete());
    }
    PG_CATCH() {
        pgcpp::error::ErrorData* ed = pgcpp::error::GetErrorData();
        SendError(ed ? ed->message : "bind error");
    }
    PG_END_TRY();
}

void Backend::HandleDescribe(DescribeKind kind, const std::string& name) {
    PG_TRY() {
        if (kind == DescribeKind::kStatement) {
            PreparedStatement* stmt = FindPreparedStatement(name);
            if (stmt == nullptr) {
                ereport(pgcpp::error::LogLevel::kError,
                        "prepared statement \"" + name + "\" does not exist");
            }

            // Send ParameterDescription.
            MessageWriter w;
            w.WriteInt16(static_cast<int16_t>(stmt->param_types.size()));
            for (Oid typoid : stmt->param_types) {
                w.WriteInt32(static_cast<int32_t>(typoid));
            }
            sink_->SendMessage(Message(MessageType::kParameterDescription, w.data()));

            // Send RowDescription or NoData.
            if (stmt->has_results && !stmt->queries.empty()) {
                SendRowDescription(stmt->queries[0]);
            } else {
                sink_->SendMessage(BuildNoData());
            }
        } else {
            // Describe portal.
            Portal* portal = FindPortal(name);
            if (portal == nullptr) {
                char errbuf[256];
                std::snprintf(errbuf, sizeof(errbuf), "portal \"%s\" does not exist", name.c_str());
                ereport(pgcpp::error::LogLevel::kError, errbuf);
            }
            if (portal->stmt != nullptr && portal->stmt->has_results &&
                portal->query_index < static_cast<int>(portal->stmt->queries.size())) {
                SendRowDescription(portal->stmt->queries[portal->query_index]);
            } else {
                sink_->SendMessage(BuildNoData());
            }
        }
    }
    PG_CATCH() {
        pgcpp::error::ErrorData* ed = pgcpp::error::GetErrorData();
        SendError(ed ? ed->message : "describe error");
    }
    PG_END_TRY();
}

void Backend::HandleExecute(const std::string& portal_name, int /*max_rows*/) {
    PG_TRY() {
        Portal* portal = FindPortal(portal_name);
        if (portal == nullptr) {
            char errbuf[256];
            std::snprintf(errbuf, sizeof(errbuf), "portal \"%s\" does not exist",
                          portal_name.c_str());
            ereport(pgcpp::error::LogLevel::kError, errbuf);
        }
        if (portal->stmt == nullptr ||
            portal->query_index >= static_cast<int>(portal->stmt->queries.size())) {
            ereport(pgcpp::error::LogLevel::kError, "portal has no query to execute");
        }

        // Convert bound parameter values from text to Datums.
        PreparedStatement* stmt = portal->stmt;
        std::vector<Datum> param_datums;
        std::vector<bool> param_isnull;
        for (std::size_t i = 0; i < portal->param_values.size(); ++i) {
            if (portal->param_isnull[i]) {
                param_datums.push_back(0);
                param_isnull.push_back(true);
            } else {
                Oid typoid = (i < stmt->param_types.size()) ? stmt->param_types[i] : kInt4Oid;
                param_datums.push_back(ConvertParamToDatum(portal->param_values[i], typoid));
                param_isnull.push_back(false);
            }
        }
        SetExecParams(param_datums, param_isnull);

        StartTransactionCommand();

        Query* query = portal->stmt->queries[portal->query_index];
        std::string tag;
        if (query->command_type == CmdType::kUtility) {
            tag = ProcessUtility(query->utility_stmt, sink_);
            if (tag.empty())
                tag = "UTILITY";
        } else {
            // Use the plan cache: get a cached plan (re-plans if invalidated
            // by DDL since the last execution).
            CachedPlanSource* plan_source = portal->stmt->plan_sources[portal->query_index];
            CachedPlan* cached_plan = plan_source->GetCachedPlan();
            if (cached_plan == nullptr) {
                ereport(pgcpp::error::LogLevel::kError, "planning failed");
            }
            // Extended query protocol: RowDescription is sent via Describe,
            // not during Execute.
            tag = ExecuteQuery(query, cached_plan->plan(),
                               /*send_row_description=*/false);
        }

        CommitTransactionCommand();
        ClearExecParams();
        sink_->SendMessage(BuildCommandComplete(tag));
    }
    PG_CATCH() {
        ClearExecParams();
        pgcpp::error::ErrorData* ed = pgcpp::error::GetErrorData();
        SendError(ed ? ed->message : "execute error");
        AbortCurrentTransaction();
    }
    PG_END_TRY();
}

void Backend::HandleSync() {
    sink_->SendMessage(BuildReadyForQuery(GetCurrentTransactionStatus()));
}

void Backend::HandleFlush() {
    // In our test sink, messages are sent immediately, so this is a no-op.
    // A real implementation would flush the socket buffer here.
}

void Backend::HandleClose(DescribeKind kind, const std::string& name) {
    if (kind == DescribeKind::kStatement) {
        auto it = prepared_statements_.find(name);
        if (it != prepared_statements_.end()) {
            delete it->second;
            prepared_statements_.erase(it);
        }
    } else {
        auto it = portals_.find(name);
        if (it != portals_.end()) {
            delete it->second;
            portals_.erase(it);
        }
    }
    sink_->SendMessage(BuildCloseComplete());
}

PreparedStatement* Backend::FindPreparedStatement(const std::string& name) const {
    auto it = prepared_statements_.find(name);
    if (it == prepared_statements_.end())
        return nullptr;
    return it->second;
}

Portal* Backend::FindPortal(const std::string& name) const {
    auto it = portals_.find(name);
    if (it == portals_.end())
        return nullptr;
    return it->second;
}

// ---------------------------------------------------------------------------
// PostgresMain — full per-connection server main loop
// ---------------------------------------------------------------------------

namespace {

// Interrupt-pending flag, set by SetInterruptPending (signal-safe).
// Checked by ProcessInterrupts at safe points in the main loop.
volatile sig_atomic_t g_interrupt_pending = 0;

// Read exactly len bytes from a fd (handles partial reads).
// Returns true on success, false on EOF or error.
bool ReadAllBytes(int fd, char* buf, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;  // EOF
        got += static_cast<std::size_t>(n);
    }
    return true;
}

// Read a single protocol message from the client.
// Returns true if a message was read, false on EOF/error.
bool ReadProtocolMessage(int fd, char* type, std::string& payload) {
    if (!ReadAllBytes(fd, type, 1))
        return false;

    char len_buf[4];
    if (!ReadAllBytes(fd, len_buf, 4))
        return false;

    int32_t length = static_cast<int32_t>(
        (static_cast<uint8_t>(len_buf[0]) << 24) | (static_cast<uint8_t>(len_buf[1]) << 16) |
        (static_cast<uint8_t>(len_buf[2]) << 8) | static_cast<uint8_t>(len_buf[3]));

    if (length < 4)
        return false;

    std::size_t payload_len = static_cast<std::size_t>(length) - 4;
    if (payload_len > 0) {
        payload.resize(payload_len);
        if (!ReadAllBytes(fd, payload.data(), payload_len))
            return false;
    } else {
        payload.clear();
    }
    return true;
}

}  // namespace

void SetInterruptPending() {
    g_interrupt_pending = 1;
}

void ProcessInterrupts() {
    if (g_interrupt_pending) {
        g_interrupt_pending = 0;
        ereport(pgcpp::error::LogLevel::kError, "canceling statement due to user request");
    }
}

void PostgresMain(int client_fd, pgcpp::server::SocketSink* sink) {
    Backend backend(sink);

    while (true) {
        char type;
        std::string payload;

        if (!ReadProtocolMessage(client_fd, &type, payload)) {
            break;  // Client disconnected or error.
        }

        // Check for interrupts at a safe point (before dispatching).
        PG_TRY() {
            ProcessInterrupts();

            MessageReader reader(payload);
            switch (type) {
                case 'Q': {
                    std::string query = reader.ReadString();
                    backend.exec_simple_query(query);
                    break;
                }
                case 'P': {
                    std::string stmt_name = reader.ReadString();
                    std::string query = reader.ReadString();
                    int16_t num_params = reader.ReadInt16();
                    std::vector<Oid> param_types;
                    param_types.reserve(static_cast<std::size_t>(num_params));
                    for (int16_t i = 0; i < num_params; ++i) {
                        param_types.push_back(static_cast<Oid>(reader.ReadInt32()));
                    }
                    backend.HandleParse(stmt_name, query, param_types);
                    break;
                }
                case 'B': {
                    std::string portal_name = reader.ReadString();
                    std::string stmt_name = reader.ReadString();
                    int16_t num_formats = reader.ReadInt16();
                    for (int16_t i = 0; i < num_formats; ++i)
                        reader.ReadInt16();
                    int16_t num_params = reader.ReadInt16();
                    std::vector<std::string> param_values;
                    std::vector<bool> param_isnull;
                    param_values.reserve(static_cast<std::size_t>(num_params));
                    param_isnull.reserve(static_cast<std::size_t>(num_params));
                    for (int16_t i = 0; i < num_params; ++i) {
                        int32_t plen = reader.ReadInt32();
                        if (plen < 0) {
                            param_values.emplace_back("");
                            param_isnull.push_back(true);
                        } else {
                            param_values.push_back(
                                reader.ReadBytes(static_cast<std::size_t>(plen)));
                            param_isnull.push_back(false);
                        }
                    }
                    int16_t num_result_formats = reader.ReadInt16();
                    for (int16_t i = 0; i < num_result_formats; ++i)
                        reader.ReadInt16();
                    backend.HandleBind(portal_name, stmt_name, param_values, param_isnull);
                    break;
                }
                case 'D': {
                    char kind = reader.ReadByte();
                    std::string name = reader.ReadString();
                    backend.HandleDescribe(static_cast<DescribeKind>(kind), name);
                    break;
                }
                case 'E': {
                    std::string portal_name = reader.ReadString();
                    int32_t max_rows = reader.ReadInt32();
                    backend.HandleExecute(portal_name, max_rows);
                    break;
                }
                case 'S': {
                    backend.HandleSync();
                    break;
                }
                case 'H': {
                    backend.HandleFlush();
                    break;
                }
                case 'C': {
                    char kind = reader.ReadByte();
                    std::string name = reader.ReadString();
                    backend.HandleClose(static_cast<DescribeKind>(kind), name);
                    break;
                }
                case 'X': {
                    // Terminate.
                    return;
                }
                default:
                    // Unknown message type — ignore.
                    break;
            }
        }
        PG_CATCH() {
            pgcpp::error::ErrorData* ed = pgcpp::error::GetErrorData();
            sink->SendMessage(BuildErrorResponse(ed ? ed->message : "internal error"));
            TransactionStatus status;
            if (IsAbortedTransactionBlock()) {
                status = TransactionStatus::kInFailedTransaction;
            } else if (IsTransactionBlock()) {
                status = TransactionStatus::kInTransaction;
            } else {
                status = TransactionStatus::kIdle;
            }
            sink->SendMessage(BuildReadyForQuery(status));
        }
        PG_END_TRY();
    }
}

}  // namespace pgcpp::protocol
