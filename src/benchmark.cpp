// benchmark.cpp
//
// A standalone throughput benchmark against the same Engine class the
// REPL uses -- no separate code path, so these numbers reflect the real
// read/write path, flush/compaction overhead included.
//
// IMPORTANT CAVEAT, read before trusting these numbers: FLUSH_THRESHOLD
// (5 entries) and COMPACTION_THRESHOLD (4 SSTables) in engine.cpp are
// deliberately tiny so flush/compaction are easy to trigger and watch by
// hand in the REPL. That means this benchmark exercises flush and
// compaction FAR more often than a real deployment would (which would
// size these in MB, not entry count) -- so the numbers below include a
// lot more flush/compaction overhead per operation than a production
// configuration would see. Useful for seeing the real cost of
// flush/compaction and the real benefit of the bloom filter, not
// directly comparable to a tuned production engine's throughput.
//
// Run with no arguments: ./lsmstore_bench

#include "engine.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

void cleanSlate() {
    for (const auto& entry : fs::directory_iterator(fs::current_path())) {
        std::string name = entry.path().filename().string();
        if (name.rfind("sstable_", 0) == 0 || name == "lsmstore.wal") {
            fs::remove(entry.path());
        }
    }
}

} // namespace

int main() {
    cleanSlate();

    constexpr int NUM_WRITES = 5000;
    constexpr int NUM_READS = 5000;

    std::cout << "lsmstore benchmark\n";
    std::cout << "===================\n";
    std::cout << NUM_WRITES << " writes, " << NUM_READS << " reads (hit), " << NUM_READS << " reads (miss)\n";
    std::cout << "flush threshold: 5 entries, compaction threshold: 4 SSTables\n";
    std::cout << "(both kept tiny for REPL demoability -- see the caveat comment at the top of this file)\n\n";

    Engine engine;

    std::vector<std::string> keys;
    keys.reserve(NUM_WRITES);
    for (int i = 0; i < NUM_WRITES; ++i) keys.push_back("key" + std::to_string(i));

    // --- Write throughput ---
    auto start = Clock::now();
    for (int i = 0; i < NUM_WRITES; ++i) {
        engine.set(keys[i], "value" + std::to_string(i));
    }
    auto end = Clock::now();
    double secs = std::chrono::duration<double>(end - start).count();
    std::cout << "Write: " << NUM_WRITES << " writes in " << secs << "s = "
              << static_cast<long>(NUM_WRITES / secs) << " writes/sec\n";
    std::cout << "Final state: " << engine.memtableEntryCount() << " entries in memtable, "
              << engine.sstableFileCount() << " SSTable file(s) on disk\n\n";

    // --- Read throughput: existing keys (mix of memtable + on-disk hits) ---
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, NUM_WRITES - 1);
    std::vector<std::string> read_keys;
    read_keys.reserve(NUM_READS);
    for (int i = 0; i < NUM_READS; ++i) read_keys.push_back(keys[dist(rng)]);

    size_t total_skipped = 0;
    size_t hits = 0;
    start = Clock::now();
    for (const auto& k : read_keys) {
        size_t skipped = 0;
        auto v = engine.get(k, skipped);
        total_skipped += skipped;
        if (v) hits++;
    }
    end = Clock::now();
    secs = std::chrono::duration<double>(end - start).count();
    std::cout << "Read (existing keys): " << NUM_READS << " reads in " << secs << "s = "
              << static_cast<long>(NUM_READS / secs) << " reads/sec ("
              << hits << "/" << NUM_READS << " hits)\n";
    std::cout << "  bloom filter skipped " << total_skipped << " file-scan(s) total (avg "
              << (static_cast<double>(total_skipped) / NUM_READS) << " per read)\n\n";

    // --- Read throughput: keys that never existed (bloom filter's best case) ---
    std::vector<std::string> miss_keys;
    miss_keys.reserve(NUM_READS);
    for (int i = 0; i < NUM_READS; ++i) miss_keys.push_back("missing_key_" + std::to_string(i));

    total_skipped = 0;
    start = Clock::now();
    for (const auto& k : miss_keys) {
        size_t skipped = 0;
        engine.get(k, skipped);
        total_skipped += skipped;
    }
    end = Clock::now();
    secs = std::chrono::duration<double>(end - start).count();
    std::cout << "Read (missing keys):  " << NUM_READS << " reads in " << secs << "s = "
              << static_cast<long>(NUM_READS / secs) << " reads/sec\n";
    std::cout << "  bloom filter skipped " << total_skipped << " file-scan(s) total (avg "
              << (static_cast<double>(total_skipped) / NUM_READS) << " per read, out of "
              << engine.sstableFileCount() << " SSTable file(s) on disk)\n";

    return 0;
}
