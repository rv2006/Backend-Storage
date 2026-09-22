#pragma once
// sstable.hpp
//
// SSTable ("Sorted String Table") — an immutable, sorted, on-disk file
// that a memtable gets flushed into once it crosses a size threshold.
//
// This is what actually turns the durable-in-memory store from Phase 1
// into an LSM-tree: data no longer has to fit in RAM, because old data
// lives in a sequence of these files on disk, and the in-memory memtable
// only ever holds the *recent* writes.
//
// Why "immutable"? SSTables are never edited in place once written.
// Updates and deletes to a key that already exists in an old SSTable are
// handled by writing a *newer* entry (in the memtable, then a newer
// SSTable) that shadows it -- readers always check newest-to-oldest and
// stop at the first match. This is also exactly why tombstones exist:
// a delete has to be written down as a real record so it can shadow an
// older value sitting in an immutable file that can't be edited.
//
// On-disk format (little-endian, fixed-width, same spirit as wal.hpp):
//
//   Header:
//     [4 bytes] magic       "SSTB"
//     [4 bytes] entry_count uint32
//   Then `entry_count` records, in ascending sorted-by-key order:
//     [1 byte]  tombstone   0 = live value, 1 = deleted
//     [4 bytes] key_len     uint32
//     [4 bytes] val_len     uint32 (0 if tombstone)
//     [key_len bytes]  key
//     [val_len bytes]  value
//
// Sorted order matters for two things we don't have yet but this format
// is designed to support later:
//   - Range scans (walk the file start to end)
//   - Early-exit point lookups (stop scanning once you pass the key,
//     since nothing after it in a sorted file can match)
//
// No index or bloom filter yet -- point lookups are a linear scan with
// early exit. Adding a bloom filter per SSTable (Phase 3) is what avoids
// opening files that provably don't contain the key at all.

#include <string>
#include <vector>
#include <optional>
#include "skiplist.hpp"

// Writes `entries` (must already be sorted ascending by key -- this is
// exactly what SkipList::entriesInOrder() gives you) to a new SSTable
// file at `path`.
void writeSSTable(const std::string& path, const std::vector<SkipList::Entry>& entries);

// Looks up `key` in the SSTable at `path`.
// Returns {value, tombstone} if found, std::nullopt if the key never
// appears in this particular file at all.
std::optional<std::pair<std::string, bool>> lookupSSTable(const std::string& path, const std::string& key);
