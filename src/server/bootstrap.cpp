// bootstrap.cpp — Bootstrap mode implementation.
//
// Converted from PostgreSQL 15's src/backend/bootstrap/bootstrap.c.
//
// Creates the data directory structure and initializes the system catalog.
// This is the pgcpp equivalent of PostgreSQL's `initdb` command.
#include "pgcpp/server/bootstrap.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <string>

#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/bootstrap_catalog.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/syscache.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/storage/bufmgr.hpp"
#include "pgcpp/storage/smgr.hpp"
#include "pgcpp/transaction/snapshot.hpp"
#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xact.hpp"

namespace pgcpp::server {

using pgcpp::access::InitializeRelcache;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::error::InitErrorSubsystem;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::MemoryContext;
using pgcpp::memory::SetCurrentMemoryContext;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;

namespace {

// Marker file written to the data directory after successful bootstrap.
constexpr const char* kBootstrapMarker = "mytoydb_version";

// Bootstrap marker content — the version of pgcpp that created the cluster.
constexpr const char* kBootstrapVersion = "mytoydb-1.0";

// Create a directory if it doesn't exist.
// Returns true on success or if the directory already exists.
bool MakeDir(const std::string& path, mode_t mode = 0700) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(path.c_str(), mode) == 0) {
        return true;
    }
    return false;
}

// Create a file with the given content.
bool WriteFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open())
        return false;
    f << content;
    return f.good();
}

// Check if a path exists (file or directory).
bool PathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

}  // namespace

BootstrapResult BootstrapCluster(const std::string& data_dir) {
    // Check that the data directory doesn't already exist.
    if (PathExists(data_dir)) {
        return BootstrapResult::kDirExists;
    }

    // Create the data directory.
    if (mkdir(data_dir.c_str(), 0700) != 0) {
        return BootstrapResult::kMkdirFailed;
    }

    // Create subdirectories.
    // base/ — per-database storage (relation files).
    // global/ — cluster-wide storage.
    // pg_wal/ — write-ahead log (not yet used, but created for structure).
    std::string base_dir = data_dir + "/base";
    std::string global_dir = data_dir + "/global";
    std::string wal_dir = data_dir + "/pg_wal";

    if (!MakeDir(base_dir) || !MakeDir(global_dir) || !MakeDir(wal_dir)) {
        return BootstrapResult::kMkdirFailed;
    }

    // Initialize the global subsystems to populate the catalog.
    InitErrorSubsystem();

    MemoryContext* top_ctx = AllocSetContext::Create("BootstrapContext");
    SetCurrentMemoryContext(top_ctx);

    Catalog* catalog = new Catalog();
    SetCatalog(catalog);
    BootstrapCatalog(catalog);

    SysCache* syscache = new SysCache();
    SetSysCache(syscache);

    ResetTransactionState();
    InitializeTransactionSystem();
    InitializeSnapshotManager();

    SetStorageBaseDir(data_dir);
    InitBufferPool(64);
    InitializeRelcache();

    // Write the bootstrap marker file.
    std::string marker_path = data_dir + "/" + kBootstrapMarker;
    if (!WriteFile(marker_path, kBootstrapVersion)) {
        // Clean up on failure.
        ResetRelcache();
        ShutdownBufferPool();
        smgrcloseall();
        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache;
        delete catalog;
        SetCurrentMemoryContext(nullptr);
        top_ctx->Delete();
        return BootstrapResult::kInitFailed;
    }

    // Clean up the subsystems (the server will re-initialize them on startup).
    ResetRelcache();
    ShutdownBufferPool();
    smgrcloseall();

    SetSysCache(nullptr);
    SetCatalog(nullptr);
    delete syscache;
    delete catalog;

    SetCurrentMemoryContext(nullptr);
    top_ctx->Delete();

    return BootstrapResult::kOk;
}

bool IsBootstrapped(const std::string& data_dir) {
    std::string marker_path = data_dir + "/" + kBootstrapMarker;
    return PathExists(marker_path);
}

const char* BootstrapResultToString(BootstrapResult result) {
    switch (result) {
        case BootstrapResult::kOk:
            return "success";
        case BootstrapResult::kDirExists:
            return "data directory already exists";
        case BootstrapResult::kMkdirFailed:
            return "failed to create directory";
        case BootstrapResult::kInitFailed:
            return "initialization failed";
        default:
            return "unknown error";
    }
}

}  // namespace pgcpp::server
