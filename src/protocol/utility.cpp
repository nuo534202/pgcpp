// utility.cpp — Utility command dispatch (ProcessUtility).
//
// Converted from PostgreSQL 15's src/backend/tcop/utility.c.
//
// This file is the thin dispatcher: it switches on the parse-tree node
// type and delegates to the appropriate handler in src/commands/*.cpp
// (tablecmds, indexcmds, copy, vacuum, sequence, view, trigger,
// explain, dbcommands, schemacmds, ...). Transaction control
// (BEGIN/COMMIT/ROLLBACK) and GUC SET/RESET are handled inline since
// they belong to the transaction and GUC modules respectively.
#include "pgcpp/protocol/utility.hpp"

#include <string>

#include "pgcpp/commands/analyze.hpp"
#include "pgcpp/commands/copy.hpp"
#include "pgcpp/commands/dbcommands.hpp"
#include "pgcpp/commands/explain.hpp"
#include "pgcpp/commands/functioncmds.hpp"
#include "pgcpp/commands/indexcmds.hpp"
#include "pgcpp/commands/schemacmds.hpp"
#include "pgcpp/commands/sequence.hpp"
#include "pgcpp/commands/tablecmds.hpp"
#include "pgcpp/commands/tablespace.hpp"
#include "pgcpp/commands/trigger.hpp"
#include "pgcpp/commands/vacuum.hpp"
#include "pgcpp/commands/view.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/transaction/xact.hpp"

namespace mytoydb::protocol {

using mytoydb::commands::AlterTable;
using mytoydb::commands::AnalyzeCommand;
using mytoydb::commands::createdb;
using mytoydb::commands::CreateFunction;
using mytoydb::commands::CreateSchemaCommand;
using mytoydb::commands::CreateTableSpace;
using mytoydb::commands::CreateTrigger;
using mytoydb::commands::DefineIndex;
using mytoydb::commands::DefineRelation;
using mytoydb::commands::DefineSequence;
using mytoydb::commands::DefineView;
using mytoydb::commands::DoCopy;
using mytoydb::commands::dropdb;
using mytoydb::commands::DropTableSpace;
using mytoydb::commands::ExecuteTruncate;
using mytoydb::commands::ExecVacuum;
using mytoydb::commands::ExplainQuery;
using mytoydb::commands::RemoveRelations;
using mytoydb::commands::RenameRelation;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::AlterTableStmt;
using mytoydb::parser::CopyStmt;
using mytoydb::parser::CreatedbStmt;
using mytoydb::parser::CreateFunctionStmt;
using mytoydb::parser::CreateSchemaStmt;
using mytoydb::parser::CreateSeqStmt;
using mytoydb::parser::CreateStmt;
using mytoydb::parser::CreateTableSpaceStmt;
using mytoydb::parser::CreateTrigStmt;
using mytoydb::parser::DropdbStmt;
using mytoydb::parser::DropStmt;
using mytoydb::parser::DropTableSpaceStmt;
using mytoydb::parser::ExplainStmt;
using mytoydb::parser::IndexStmt;
using mytoydb::parser::Node;
using mytoydb::parser::RenameStmt;
using mytoydb::parser::TransactionStmt;
using mytoydb::parser::TruncateStmt;
using mytoydb::parser::VacuumStmt;
using mytoydb::parser::VariableSetStmt;
using mytoydb::parser::ViewStmt;

namespace {

// ProcessTransactionStmt — BEGIN / COMMIT / ROLLBACK.
// (Transaction control lives in the transaction module; this is a thin
// wrapper kept here because tcop/utility.c also dispatches these inline.)
std::string ProcessTransactionStmt(TransactionStmt* stmt) {
    switch (stmt->kind) {
        case TransactionStmt::Kind::kBegin:
        case TransactionStmt::Kind::kStart:
            mytoydb::transaction::BeginTransactionBlock();
            return "BEGIN";
        case TransactionStmt::Kind::kCommit:
            mytoydb::transaction::EndTransactionBlock();
            return "COMMIT";
        case TransactionStmt::Kind::kRollback:
            mytoydb::transaction::AbortTransactionBlock();
            return "ROLLBACK";
        default:
            return "";
    }
}

// ProcessVariableSetStmt — SET / RESET (GUC).
// (GUC handling lives in the GUC module; this is a thin stub since most
// SET commands just record the value for the current session.)
std::string ProcessVariableSetStmt(VariableSetStmt* stmt) {
    if (stmt == nullptr)
        return "SET";
    switch (stmt->kind) {
        case VariableSetStmt::Kind::kReset:
        case VariableSetStmt::Kind::kResetAll:
            return "RESET";
        default:
            return "SET";
    }
}

}  // namespace

// --- Public API ---

std::string ProcessUtility(Node* stmt, OutputSink* /*sink*/) {
    if (stmt == nullptr)
        return "";

    switch (stmt->GetTag()) {
        // --- Transaction & GUC (handled inline) -------------------------
        case NodeTag::kTransactionStmt:
            return ProcessTransactionStmt(static_cast<TransactionStmt*>(stmt));
        case NodeTag::kVariableSetStmt:
            return ProcessVariableSetStmt(static_cast<VariableSetStmt*>(stmt));

        // --- Table / Index DDL (commands/tablecmds, indexcmds) --------
        case NodeTag::kCreateStmt:
            return DefineRelation(static_cast<CreateStmt*>(stmt));
        case NodeTag::kDropStmt:
            return RemoveRelations(static_cast<DropStmt*>(stmt));
        case NodeTag::kAlterTableStmt:
            return AlterTable(static_cast<AlterTableStmt*>(stmt));
        case NodeTag::kRenameStmt:
            return RenameRelation(static_cast<RenameStmt*>(stmt));
        case NodeTag::kTruncateStmt:
            return ExecuteTruncate(static_cast<TruncateStmt*>(stmt));
        case NodeTag::kIndexStmt:
            return DefineIndex(static_cast<IndexStmt*>(stmt));

        // --- COPY (commands/copy) -------------------------------------
        case NodeTag::kCopyStmt:
            return DoCopy(static_cast<CopyStmt*>(stmt));

        // --- VACUUM / ANALYZE (commands/vacuum, analyze) -------------
        case NodeTag::kVacuumStmt: {
            auto* v = static_cast<VacuumStmt*>(stmt);
            return v->is_vacuumcmd ? ExecVacuum(v) : AnalyzeCommand(v);
        }

        // --- Sequence / View / Trigger (commands/*) -------------------
        case NodeTag::kCreateSeqStmt:
            return DefineSequence(static_cast<CreateSeqStmt*>(stmt));
        case NodeTag::kViewStmt:
            return DefineView(static_cast<ViewStmt*>(stmt));
        case NodeTag::kCreateTrigStmt:
            return CreateTrigger(static_cast<CreateTrigStmt*>(stmt));

        // --- EXPLAIN (commands/explain) -------------------------------
        case NodeTag::kExplainStmt:
            return ExplainQuery(static_cast<ExplainStmt*>(stmt));

        // --- Database / Schema / Tablespace (commands/*) -------------
        case NodeTag::kCreatedbStmt:
            return createdb(static_cast<CreatedbStmt*>(stmt));
        case NodeTag::kDropdbStmt:
            return dropdb(static_cast<DropdbStmt*>(stmt));
        case NodeTag::kCreateSchemaStmt:
            return CreateSchemaCommand(static_cast<CreateSchemaStmt*>(stmt));
        case NodeTag::kCreateTableSpaceStmt:
            return CreateTableSpace(static_cast<CreateTableSpaceStmt*>(stmt));
        case NodeTag::kDropTableSpaceStmt:
            return DropTableSpace(static_cast<DropTableSpaceStmt*>(stmt));

        // --- Function / Type / Operator / OpClass / Aggregate --------
        case NodeTag::kCreateFunctionStmt:
            return CreateFunction(static_cast<CreateFunctionStmt*>(stmt));
        // Note: CREATE TYPE / OPERATOR / OPERATOR CLASS / AGGREGATE are
        // parsed as CreateStmt in PostgreSQL (they share the grammar
        // production). We can't distinguish them from CREATE TABLE by node
        // tag alone, so they fall through to DefineRelation — which will
        // create a regular table. A future task will add distinct node
        // types or a discriminator field so these can route correctly.
        default:
            return "";
    }
}

std::string CreateCommandTag(Node* stmt) {
    if (stmt == nullptr)
        return "";
    switch (stmt->GetTag()) {
        case NodeTag::kTransactionStmt: {
            auto* ts = static_cast<TransactionStmt*>(stmt);
            switch (ts->kind) {
                case TransactionStmt::Kind::kBegin:
                case TransactionStmt::Kind::kStart:
                    return "BEGIN";
                case TransactionStmt::Kind::kCommit:
                    return "COMMIT";
                case TransactionStmt::Kind::kRollback:
                    return "ROLLBACK";
                default:
                    return "BEGIN";
            }
        }
        case NodeTag::kCreateStmt:
            return "CREATE TABLE";
        case NodeTag::kDropStmt: {
            auto* d = static_cast<DropStmt*>(stmt);
            switch (d->remove_type) {
                case mytoydb::parser::ObjectType::kTable:
                    return "DROP TABLE";
                case mytoydb::parser::ObjectType::kIndex:
                    return "DROP INDEX";
                case mytoydb::parser::ObjectType::kView:
                    return "DROP VIEW";
                default:
                    return "DROP";
            }
        }
        case NodeTag::kAlterTableStmt:
            return "ALTER TABLE";
        case NodeTag::kRenameStmt:
            return "ALTER TABLE";
        case NodeTag::kIndexStmt:
            return "CREATE INDEX";
        case NodeTag::kTruncateStmt:
            return "TRUNCATE TABLE";
        case NodeTag::kVacuumStmt: {
            auto* v = static_cast<VacuumStmt*>(stmt);
            return v->is_vacuumcmd ? "VACUUM" : "ANALYZE";
        }
        case NodeTag::kCopyStmt:
            return "COPY";
        case NodeTag::kVariableSetStmt: {
            auto* s = static_cast<VariableSetStmt*>(stmt);
            return (s->kind == VariableSetStmt::Kind::kReset ||
                    s->kind == VariableSetStmt::Kind::kResetAll)
                       ? "RESET"
                       : "SET";
        }
        case NodeTag::kCreateSeqStmt:
            return "CREATE SEQUENCE";
        case NodeTag::kViewStmt:
            return "CREATE VIEW";
        case NodeTag::kCreateTrigStmt:
            return "CREATE TRIGGER";
        case NodeTag::kExplainStmt:
            return "EXPLAIN";
        case NodeTag::kCreatedbStmt:
            return "CREATE DATABASE";
        case NodeTag::kDropdbStmt:
            return "DROP DATABASE";
        case NodeTag::kCreateSchemaStmt:
            return "CREATE SCHEMA";
        case NodeTag::kCreateTableSpaceStmt:
            return "CREATE TABLESPACE";
        case NodeTag::kDropTableSpaceStmt:
            return "DROP TABLESPACE";
        case NodeTag::kCreateFunctionStmt:
            return "CREATE FUNCTION";
        default:
            return "";
    }
}

bool UtilityReturnsTuples(Node* /*stmt*/) {
    // EXPLAIN and VACUUM VERBOSE return tuples in PostgreSQL; MyToyDB's
    // EXPLAIN stub prints to stdout instead of returning rows.
    return false;
}

}  // namespace mytoydb::protocol
