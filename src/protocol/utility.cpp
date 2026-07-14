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
#include "protocol/utility.hpp"

#include <string>

#include "commands/analyze.hpp"
#include "commands/castcmds.hpp"
#include "commands/copy.hpp"
#include "commands/dbcommands.hpp"
#include "commands/domaincmds.hpp"
#include "commands/explain.hpp"
#include "commands/functioncmds.hpp"
#include "commands/indexcmds.hpp"
#include "commands/schemacmds.hpp"
#include "commands/sequence.hpp"
#include "commands/tablecmds.hpp"
#include "commands/tablespace.hpp"
#include "commands/trigger.hpp"
#include "commands/typecmds.hpp"
#include "commands/vacuum.hpp"
#include "commands/view.hpp"
#include "common/containers/node.hpp"
#include "parser/parsenodes.hpp"
#include "transaction/twophase.hpp"
#include "transaction/xact.hpp"
#include "utils/cache/inval.hpp"

namespace pgcpp::protocol {

using pgcpp::commands::AlterTable;
using pgcpp::commands::AnalyzeCommand;
using pgcpp::commands::CreateCast;
using pgcpp::commands::createdb;
using pgcpp::commands::CreateFunction;
using pgcpp::commands::CreateSchemaCommand;
using pgcpp::commands::CreateTableSpace;
using pgcpp::commands::CreateTrigger;
using pgcpp::commands::DefineDomain;
using pgcpp::commands::DefineIndex;
using pgcpp::commands::DefineRelation;
using pgcpp::commands::DefineSequence;
using pgcpp::commands::DefineType;
using pgcpp::commands::DefineView;
using pgcpp::commands::DoCopy;
using pgcpp::commands::dropdb;
using pgcpp::commands::DropTableSpace;
using pgcpp::commands::ExecuteTruncate;
using pgcpp::commands::ExecVacuum;
using pgcpp::commands::ExplainQuery;
using pgcpp::commands::RemoveRelations;
using pgcpp::commands::RenameRelation;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::AlterTableStmt;
using pgcpp::parser::CopyStmt;
using pgcpp::parser::CreateCastStmt;
using pgcpp::parser::CreatedbStmt;
using pgcpp::parser::CreateDomainStmt;
using pgcpp::parser::CreateFunctionStmt;
using pgcpp::parser::CreateSchemaStmt;
using pgcpp::parser::CreateSeqStmt;
using pgcpp::parser::CreateStmt;
using pgcpp::parser::CreateTableSpaceStmt;
using pgcpp::parser::CreateTrigStmt;
using pgcpp::parser::CreateTypeStmt;
using pgcpp::parser::DropdbStmt;
using pgcpp::parser::DropStmt;
using pgcpp::parser::DropTableSpaceStmt;
using pgcpp::parser::ExplainStmt;
using pgcpp::parser::IndexStmt;
using pgcpp::parser::Node;
using pgcpp::parser::RenameStmt;
using pgcpp::parser::TransactionStmt;
using pgcpp::parser::TruncateStmt;
using pgcpp::parser::VacuumStmt;
using pgcpp::parser::VariableSetStmt;
using pgcpp::parser::ViewStmt;

namespace {

// ApplyTransactionOptions — apply DefElems produced by the grammar's
// opt_transaction_mode_list (ISOLATION LEVEL, READ ONLY, DEFERRABLE)
// to the current transaction. Called right after BeginTransactionBlock.
void ApplyTransactionOptions(const std::vector<pgcpp::parser::Node*>& options) {
    for (pgcpp::parser::Node* opt : options) {
        auto* def = dynamic_cast<pgcpp::parser::DefElem*>(opt);
        if (def == nullptr || def->arg == nullptr) {
            continue;
        }
        if (def->defname == "transaction_isolation") {
            const auto* v = dynamic_cast<const pgcpp::nodes::Value*>(def->arg);
            if (v != nullptr) {
                pgcpp::transaction::SetTransactionIsolationLevel(
                    pgcpp::transaction::ParseIsolationLevelName(v->GetString()));
            }
        } else if (def->defname == "transaction_read_only") {
            const auto* v = dynamic_cast<const pgcpp::nodes::Value*>(def->arg);
            if (v != nullptr) {
                pgcpp::transaction::SetTransactionReadOnly(v->GetInteger() != 0);
            }
        } else if (def->defname == "transaction_deferrable") {
            const auto* v = dynamic_cast<const pgcpp::nodes::Value*>(def->arg);
            if (v != nullptr) {
                pgcpp::transaction::SetTransactionDeferrable(v->GetInteger() != 0);
            }
        }
    }
}

// ProcessTransactionStmt — BEGIN / COMMIT / ROLLBACK / PREPARE / COMMIT PREPARED / ROLLBACK
// PREPARED. (Transaction control lives in the transaction module; this is a thin wrapper kept here
// because tcop/utility.c also dispatches these inline.)
std::string ProcessTransactionStmt(TransactionStmt* stmt) {
    switch (stmt->kind) {
        case TransactionStmt::Kind::kBegin:
        case TransactionStmt::Kind::kStart:
            pgcpp::transaction::BeginTransactionBlock();
            ApplyTransactionOptions(stmt->options);
            return "BEGIN";
        case TransactionStmt::Kind::kCommit:
            pgcpp::transaction::EndTransactionBlock();
            return "COMMIT";
        case TransactionStmt::Kind::kRollback:
            pgcpp::transaction::AbortTransactionBlock();
            return "ROLLBACK";
        case TransactionStmt::Kind::kPrepare:
            pgcpp::transaction::PrepareTransactionBlock(stmt->gid);
            return "PREPARE TRANSACTION";
        case TransactionStmt::Kind::kCommitPrepared:
            pgcpp::transaction::CommitPreparedTransaction(stmt->gid);
            return "COMMIT PREPARED";
        case TransactionStmt::Kind::kRollbackPrepared:
            pgcpp::transaction::RollbackPreparedTransaction(stmt->gid);
            return "ROLLBACK PREPARED";
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

std::string ProcessUtility(Node* stmt, OutputSink* sink) {
    if (stmt == nullptr)
        return "";

    std::string tag;
    switch (stmt->GetTag()) {
        // --- Transaction & GUC (handled inline, NOT DDL) --------------
        case NodeTag::kTransactionStmt:
            return ProcessTransactionStmt(static_cast<TransactionStmt*>(stmt));
        case NodeTag::kVariableSetStmt:
            return ProcessVariableSetStmt(static_cast<VariableSetStmt*>(stmt));

        // --- Table / Index DDL (commands/tablecmds, indexcmds) --------
        case NodeTag::kCreateStmt:
            tag = DefineRelation(static_cast<CreateStmt*>(stmt));
            break;
        case NodeTag::kDropStmt:
            tag = RemoveRelations(static_cast<DropStmt*>(stmt));
            break;
        case NodeTag::kAlterTableStmt:
            tag = AlterTable(static_cast<AlterTableStmt*>(stmt));
            break;
        case NodeTag::kRenameStmt:
            tag = RenameRelation(static_cast<RenameStmt*>(stmt));
            break;
        case NodeTag::kTruncateStmt:
            tag = ExecuteTruncate(static_cast<TruncateStmt*>(stmt));
            break;
        case NodeTag::kIndexStmt:
            tag = DefineIndex(static_cast<IndexStmt*>(stmt));
            break;

        // --- COPY (commands/copy, NOT DDL) ---------------------------
        case NodeTag::kCopyStmt:
            return DoCopy(static_cast<CopyStmt*>(stmt));

        // --- VACUUM / ANALYZE (commands/vacuum, analyze, NOT DDL) ----
        case NodeTag::kVacuumStmt: {
            auto* v = static_cast<VacuumStmt*>(stmt);
            return v->is_vacuumcmd ? ExecVacuum(v) : AnalyzeCommand(v);
        }

        // --- Sequence / View / Trigger (commands/*) -------------------
        case NodeTag::kCreateSeqStmt:
            tag = DefineSequence(static_cast<CreateSeqStmt*>(stmt));
            break;
        case NodeTag::kViewStmt:
            tag = DefineView(static_cast<ViewStmt*>(stmt));
            break;
        case NodeTag::kCreateTrigStmt:
            tag = CreateTrigger(static_cast<CreateTrigStmt*>(stmt));
            break;

        // --- EXPLAIN (commands/explain, NOT DDL) ---------------------
        case NodeTag::kExplainStmt:
            return ExplainQuery(static_cast<ExplainStmt*>(stmt), sink);

        // --- Database / Schema / Tablespace (commands/*) -------------
        case NodeTag::kCreatedbStmt:
            tag = createdb(static_cast<CreatedbStmt*>(stmt));
            break;
        case NodeTag::kDropdbStmt:
            tag = dropdb(static_cast<DropdbStmt*>(stmt));
            break;
        case NodeTag::kCreateSchemaStmt:
            tag = CreateSchemaCommand(static_cast<CreateSchemaStmt*>(stmt));
            break;
        case NodeTag::kCreateTableSpaceStmt:
            tag = CreateTableSpace(static_cast<CreateTableSpaceStmt*>(stmt));
            break;
        case NodeTag::kDropTableSpaceStmt:
            tag = DropTableSpace(static_cast<DropTableSpaceStmt*>(stmt));
            break;

        // --- Function / Type / Operator / OpClass / Aggregate --------
        case NodeTag::kCreateFunctionStmt:
            tag = CreateFunction(static_cast<CreateFunctionStmt*>(stmt));
            break;
        case NodeTag::kCreateTypeStmt:
            tag = DefineType(static_cast<CreateTypeStmt*>(stmt));
            break;
        case NodeTag::kCreateDomainStmt:
            tag = DefineDomain(static_cast<CreateDomainStmt*>(stmt));
            break;
        case NodeTag::kCreateCastStmt:
            tag = CreateCast(static_cast<CreateCastStmt*>(stmt));
            break;
        // Note: CREATE TYPE / OPERATOR / OPERATOR CLASS / AGGREGATE are
        // parsed as CreateStmt in PostgreSQL (they share the grammar
        // production). We can't distinguish them from CREATE TABLE by node
        // tag alone, so they fall through to DefineRelation — which will
        // create a regular table. A future task will add distinct node
        // types or a discriminator field so these can route correctly.
        default:
            return "";
    }

    // DDL succeeded — invalidate cached plans by bumping the catalog
    // generation counter. Non-DDL commands (transaction, GUC, COPY, VACUUM,
    // EXPLAIN) return early above and don't reach this point.
    pgcpp::utils::IncrementCatalogGeneration();
    return tag;
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
                case TransactionStmt::Kind::kPrepare:
                    return "PREPARE TRANSACTION";
                case TransactionStmt::Kind::kCommitPrepared:
                    return "COMMIT PREPARED";
                case TransactionStmt::Kind::kRollbackPrepared:
                    return "ROLLBACK PREPARED";
                default:
                    return "BEGIN";
            }
        }
        case NodeTag::kCreateStmt:
            return "CREATE TABLE";
        case NodeTag::kDropStmt: {
            auto* d = static_cast<DropStmt*>(stmt);
            switch (d->remove_type) {
                case pgcpp::parser::ObjectType::kTable:
                    return "DROP TABLE";
                case pgcpp::parser::ObjectType::kIndex:
                    return "DROP INDEX";
                case pgcpp::parser::ObjectType::kView:
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
        case NodeTag::kCreateTypeStmt:
            return "CREATE TYPE";
        case NodeTag::kCreateDomainStmt:
            return "CREATE DOMAIN";
        case NodeTag::kCreateCastStmt:
            return "CREATE CAST";
        default:
            return "";
    }
}

bool UtilityReturnsTuples(Node* stmt) {
    // EXPLAIN returns tuples (a single "QUERY PLAN" text column).
    if (stmt != nullptr && stmt->GetTag() == NodeTag::kExplainStmt)
        return true;
    return false;
}

}  // namespace pgcpp::protocol
