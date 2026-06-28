// origin.h — Replication origin tracking.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/origin.c.
//
// A "replication origin" identifies the upstream node that produced a
// given change. Each origin is assigned a small (uint16) ID local to
// this node. PG stores origins in pg_replication_origin catalog and
// keeps a session-local "current origin" used to stamp incoming records.
//
// pgcpp keeps an in-process std::map<uint16, CommitOrigin> and tracks
// the current session's origin id and LSN.
#pragma once

#include <cstdint>
#include <string>

#include "pgcpp/transaction/xlog.hpp"

namespace pgcpp::replication {

// RepOriginId — small integer identifying an origin (PG: RepOriginId).
using RepOriginId = uint16_t;

// kInvalidRepOriginId — sentinel for "no origin" (PG: InvalidRepOriginId).
constexpr RepOriginId kInvalidRepOriginId = 0;
// First valid origin id (PG reserves 0..16 for builtins, e.g.).
constexpr RepOriginId kFirstUserRepOriginId = 17;

// CommitOrigin — one origin's tracked state.
// PG stores this in a shmem array indexed by RepOriginId; we use a map.
struct CommitOrigin {
    RepOriginId id = kInvalidRepOriginId;
    std::string name;                        // human-readable name (e.g. "node_a")
    transaction::XLogRecPtr remote_lsn = 0;  // commit LSN at the origin
    transaction::XLogRecPtr local_lsn = 0;   // local LSN at which it was applied
};

// ReplOriginInit — initialize the origin subsystem.
void ReplOriginInit();

// ReplOriginReset — clear all origins (tests).
void ReplOriginReset();

// ReploriginCreate — register a new origin with the given name. Returns
// the new origin's id. If an origin with the same name already exists,
// returns its existing id (PG behavior).
RepOriginId ReploriginCreate(const std::string& name);

// ReploriginDrop — drop an origin by id. Returns false if missing.
bool ReploriginDrop(RepOriginId id);

// ReploriginDropByName — drop an origin by name. Returns false if missing.
bool ReploriginDropByName(const std::string& name);

// ReploriginAdvance — advance the (remote_lsn, local_lsn) of an origin.
// Returns false if the origin is missing.
bool ReploriginAdvance(RepOriginId id, transaction::XLogRecPtr remote_lsn,
                       transaction::XLogRecPtr local_lsn);

// ReploriginSessionSet — set the current session's origin (must exist).
// Subsequent applies are stamped with this id. Returns false if missing.
bool ReploriginSessionSet(RepOriginId id);

// ReploriginSessionGet — return the current session's origin id (or
// kInvalidRepOriginId if none set).
RepOriginId ReploriginSessionGet();

// ReploriginSessionReset — clear the current session's origin.
void ReploriginSessionReset();

// ReploriginGet — return a pointer to the origin with the given id (or
// nullptr if missing).
const CommitOrigin* ReploriginGet(RepOriginId id);

// ReploriginGetByName — return id for name, or kInvalidRepOriginId.
RepOriginId ReploriginGetByName(const std::string& name);

// ReploriginCount — number of registered origins.
int ReploriginCount();

// ReploriginSessionLsn — return the current session's local LSN stamp.
transaction::XLogRecPtr ReploriginSessionLsn();

}  // namespace pgcpp::replication
