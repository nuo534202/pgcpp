// worker.cpp — Logical replication apply worker.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/worker.c.
// MyToyDB keeps a small in-process pool (std::vector<LogicalRepWorker>)
// and runs workers synchronously in ApplyWorkerMain (no fork).
#include "pgcpp/replication/worker.hpp"

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace mytoydb::replication {

using mytoydb::error::LogLevel;

namespace {

LogicalRepWorkerPool& Pool() {
    static LogicalRepWorkerPool p;
    return p;
}

}  // namespace

void LogicalRepWorkerInit() {
    Pool() = LogicalRepWorkerPool{};
}

void LogicalRepWorkerReset() {
    LogicalRepWorkerInit();
}

int LogicalRepWorkerAdd(int32_t subid, int32_t relid, LogicalRepWorkerType type,
                        std::string subscription_name) {
    if (subid <= 0) {
        ereport(LogLevel::kError, "LogicalRepWorkerAdd: subid is invalid");
        return -1;
    }
    // Reuse a freed slot if possible.
    for (std::size_t i = 0; i < Pool().workers.size(); ++i) {
        if (!Pool().workers[i].in_use) {
            LogicalRepWorker& w = Pool().workers[i];
            w = LogicalRepWorker{};
            w.subid = subid;
            w.relid = relid;
            w.type = type;
            w.subscription_name = std::move(subscription_name);
            w.in_use = true;
            w.pid = static_cast<int32_t>(i + 1);
            return static_cast<int>(i);
        }
    }
    if (static_cast<int>(Pool().workers.size()) >= Pool().max_workers) {
        ereport(LogLevel::kError, "LogicalRepWorkerAdd: pool is full");
        return -1;
    }
    LogicalRepWorker w;
    w.subid = subid;
    w.relid = relid;
    w.type = type;
    w.subscription_name = std::move(subscription_name);
    w.in_use = true;
    w.pid = static_cast<int32_t>(Pool().workers.size() + 1);
    Pool().workers.push_back(std::move(w));
    return static_cast<int>(Pool().workers.size()) - 1;
}

bool LogicalRepWorkerRemove(int idx) {
    if (idx < 0 || idx >= static_cast<int>(Pool().workers.size())) {
        return false;
    }
    LogicalRepWorker& w = Pool().workers[static_cast<std::size_t>(idx)];
    if (!w.in_use) {
        return false;
    }
    w.in_use = false;
    w.running = false;
    w.pid = 0;
    return true;
}

int LogicalRepWorkerFindBySub(int32_t subid) {
    for (std::size_t i = 0; i < Pool().workers.size(); ++i) {
        if (Pool().workers[i].in_use && Pool().workers[i].subid == subid &&
            Pool().workers[i].type == LogicalRepWorkerType::kApply) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

LogicalRepWorker* LogicalRepWorkerGetByIndex(int idx) {
    if (idx < 0 || idx >= static_cast<int>(Pool().workers.size())) {
        return nullptr;
    }
    LogicalRepWorker& w = Pool().workers[static_cast<std::size_t>(idx)];
    if (!w.in_use) {
        return nullptr;
    }
    return &w;
}

int LogicalRepWorkerCount() {
    int n = 0;
    for (const auto& w : Pool().workers) {
        if (w.in_use) {
            ++n;
        }
    }
    return n;
}

int ApplyWorkerMain(int idx) {
    LogicalRepWorker* w = LogicalRepWorkerGetByIndex(idx);
    if (w == nullptr) {
        ereport(LogLevel::kError, "ApplyWorkerMain: invalid worker index");
        return -1;
    }
    w->running = true;
    // Stubbed: in real PG the worker pulls messages off the subscription's
    // replication slot and applies them. We just advance the commit LSN to
    // the current WAL insert position to simulate progress.
    w->commit_lsn = transaction::GetXLogInsertRecPtr();
    w->running = false;
    return 0;
}

void ApplyWorkerWakeup(int idx) {
    LogicalRepWorker* w = LogicalRepWorkerGetByIndex(idx);
    if (w != nullptr) {
        w->running = true;
    }
}

LogicalRepWorkerPool* GetLogicalRepWorkerPool() {
    return &Pool();
}

}  // namespace mytoydb::replication
