// block.h — Block number and page size constants.
//
// Converted from PostgreSQL 15's src/include/storage/block.h.
// Defines the fundamental addressing unit of storage: the block (page).
#pragma once

#include <cstdint>

namespace mytoydb::storage {

// BlockNumber — the address of a page within a relation file.
// Equivalent to PostgreSQL's BlockNumber (uint32_t).
using BlockNumber = uint32_t;

// InvalidBlockNumber — sentinel value meaning "no such block".
constexpr BlockNumber kInvalidBlockNumber = 0xFFFFFFFF;

// BlockNumberIsValid — true if the block number is a valid address.
inline bool BlockNumberIsValid(BlockNumber block_num) {
    return block_num != kInvalidBlockNumber;
}

// BLCKSZ — the size of a page in bytes. PostgreSQL's default is 8192.
constexpr int kBlckSz = 8192;

// RELSEG_SIZE — number of blocks per segment file. PostgreSQL's default is
// 131072 (1 GB / 8 KB). MyToyDB uses the same value for fidelity.
constexpr int kRelSegSize = 131072;

// MaxBlockNumber — the highest valid BlockNumber.
constexpr BlockNumber kMaxBlockNumber = 0xFFFFFFFE;

}  // namespace mytoydb::storage
