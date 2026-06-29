// bootstrap.h — Bootstrap mode: initialize a new database cluster.
//
// Converted from PostgreSQL 15's src/backend/bootstrap/bootstrap.c.
//
// The bootstrap mode is the pgcpp equivalent of PostgreSQL's `initdb`.
// It creates the data directory structure and initializes the system
// catalog so that the server can start and accept connections.
#pragma once

#include <string>

namespace pgcpp::server {

// BootstrapResult — outcome of a bootstrap operation.
enum class BootstrapResult {
    kOk,
    kDirExists,
    kMkdirFailed,
    kInitFailed,
};

// BootstrapCluster — initialize a new database cluster.
//
// Creates the data directory and subdirectories, initializes the
// system catalog with built-in types/operators/functions, and writes
// a marker file indicating a successful bootstrap.
//
// Parameters:
//   data_dir — the path to the data directory to create.
//
// Returns kOk on success, or an error code on failure.
BootstrapResult BootstrapCluster(const std::string& data_dir);

// IsBootstrapped — check if a data directory has been bootstrapped.
// Returns true if the directory contains a valid bootstrap marker.
bool IsBootstrapped(const std::string& data_dir);

// GetBootstrapError — return a human-readable description of the last
// bootstrap error.
const char* BootstrapResultToString(BootstrapResult result);

}  // namespace pgcpp::server
