#pragma once
// compaction.hpp
//
// Compaction: merge several SSTables into one, so old files stop piling
// up forever and reads stop having to check more and more of them.
//
// This implementation is deliberately the simplest correct version:
// "full" compaction, which merges ALL current SSTables into a single new
// one in one shot. Real engines (RocksDB, LevelDB) use tiered/leveled
// strategies that compact small groups of files incrementally instead,
// because merging everything at once gets expensive as the dataset
// grows -- but the core merge logic (the actually hard part) is the same
// either way. Compacting only a subset of files at a time is a natural
// "phase 2" extension once this works.
//
// The two things a merge has to get right:
//
// 1. When the same key appears in multiple input files, only the
//    newest version survives. That's why `mergeSSTables` takes its
//    input paths ordered newest-to-oldest -- same convention as GET's
//    search order -- and keeps the first occurrence of each key it
//    sees, discarding the rest.
//
// 2. A tombstone can only be safely dropped once there's no older file
//    left that it could still be shadowing. Since this is a *full*
//    compaction (every SSTable is being merged together, with nothing
//    left outside the merge), any tombstone surviving into the merged
//    output has nothing left to shadow -- so tombstones are dropped
//    entirely from the result. If this ever becomes a *partial*
//    compaction (only some files merged, others left untouched), this
//    would have to change: a tombstone would need to be kept unless the
//    merge is known to include every file older than it.

#include <string>
#include <vector>
#include "skiplist.hpp"

struct CompactionResult {
    std::vector<SkipList::Entry> entries; // sorted ascending, tombstones dropped
    size_t input_entry_count = 0;         // total entries read across all inputs, before dedup
    size_t tombstones_dropped = 0;
};

// Merges the SSTables at `sstable_paths` (must be ordered newest-first,
// matching GET's search order) into a single sorted entry list with
// duplicates resolved (newest wins) and tombstones removed.
// Does not touch any files -- the caller is responsible for writing the
// result to a new SSTable and deleting the old ones.
CompactionResult mergeSSTables(const std::vector<std::string>& sstable_paths);
