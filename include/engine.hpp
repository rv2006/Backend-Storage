#pragma once
// engine.hpp
//
// The storage engine's core, as a reusable class: everything main.cpp's
// REPL used to do inline (WAL + memtable + SSTable flush + bloom filter
// + compaction) now lives here instead, so it can be driven by more than
// just a stdin loop.
//
// Why this refactor, and why now: up through Phase 4, everything lived
// directly in main()'s local variables and lambdas, which was fine for a
// REPL but meant the only way to exercise the engine was by typing
// commands one at a time. Benchmarking needs to drive thousands of
// operations in a tight timed loop -- that needs a real API, not stdin.
// Pulling the engine out into its own class is what makes that possible,
// and it's also just better structure: main.cpp is now a thin REPL that
// calls into Engine, and a new benchmark.cpp can do the same without
// duplicating any of the WAL/memtable/SSTable/compaction logic.
//
// No behavior changed in this refactor -- it's the same read/write path,
// same flush/compaction thresholds, same on-disk formats, just moved
// out from inline main() code into methods on this class.

#include "skiplist.hpp"
#include "wal.hpp"
#include "bloomfilter.hpp"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <optional>
#include <functional>

class Engine {
public:
    // Opens (or creates) an engine rooted at the current working
    // directory: WAL file, SSTable files, and filter sidecars all live
    // alongside each other there, same convention as before this
    // refactor. Replays the WAL and rediscovers existing SSTables as
    // part of construction, same as main() used to do at startup.
    Engine();

    void set(const std::string& key, const std::string& value);
    void remove(const std::string& key);

    // Returns the live value if present, std::nullopt if the key is
    // absent or was deleted (tombstoned). This intentionally collapses
    // "never existed" and "was deleted" into one case for callers that
    // just want "do I have a usable value for this key" -- same
    // semantics the REPL's GET already had.
    std::optional<std::string> get(const std::string& key);

    // Same lookup as get(), but also reports how many SSTable files the
    // bloom filters let it skip -- used by the REPL to print that, and
    // useful for a benchmark that wants to report filter effectiveness.
    std::optional<std::string> get(const std::string& key, size_t& skipped_via_filter);

    size_t memtableEntryCount() const;
    size_t sstableFileCount() const;

    // Optional hooks so a caller (the REPL) can print what just
    // happened without Engine itself knowing anything about stdout --
    // the benchmark harness just leaves these unset and runs silently.
    // on_flush(sstable_path); on_compaction(files_before, new_path,
    // entries_in, entries_out, tombstones_dropped).
    std::function<void(const std::string&)> on_flush;
    std::function<void(size_t, const std::string&, size_t, size_t, size_t)> on_compaction;

    // How many WAL records were replayed at startup (0 on a fresh/clean
    // start). Exposed so the REPL can print its existing recovery
    // message without needing to duplicate the counting logic.
    size_t recoveredRecordCount() const { return recovered_count_; }
    size_t discoveredSSTableCount() const { return discovered_sstable_count_; }
    size_t discoveredFilterCount() const { return discovered_filter_count_; }

private:
    std::unique_ptr<SkipList> memtable_;
    WriteAheadLog wal_;
    int next_sstable_id_ = 0;
    std::vector<std::string> sstables_; // newest-first
    std::unordered_map<std::string, BloomFilter> filters_;

    size_t recovered_count_ = 0;
    size_t discovered_sstable_count_ = 0;
    size_t discovered_filter_count_ = 0;

    void flushMemtable();
    void compactIfNeeded();
};
