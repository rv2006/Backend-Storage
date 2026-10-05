// main.cpp
//
// A REPL on top of Engine (engine.hpp/engine.cpp), which is where the
// actual storage engine core lives: WAL (durability) + SkipList
// (in-memory memtable) + SSTable (on-disk flush) + BloomFilter (skip
// SSTables that provably don't contain a key) + Compaction (merge old
// SSTables back into one).
//
// This completes the core LSM-tree loop: memtable -> WAL -> SSTable ->
// bloom filter -> compaction. See benchmark.cpp for throughput numbers
// against the same Engine class, and README.md for the full writeup.
//
// Commands:
//   SET <key> <value>
//   GET <key>
//   DEL <key>
//   COUNT
//   EXIT

#include "engine.hpp"
#include <iostream>
#include <sstream>

int main() {
    Engine engine;
    engine.on_flush = [](const std::string& path) {
        std::cout << "(flushed memtable -> " << path << ", bloom filter written)\n";
    };
    engine.on_compaction = [](size_t files_before, const std::string& new_path,
                               size_t entries_in, size_t entries_out, size_t tombstones_dropped) {
        std::cout << "(compacted " << files_before << " SSTables -> " << new_path
                   << ": " << entries_in << " entries in -> " << entries_out
                   << " live entries out, " << tombstones_dropped << " tombstone(s) dropped)\n";
    };

    if (engine.discoveredSSTableCount() > 0) {
        std::cout << "Found " << engine.discoveredSSTableCount() << " existing SSTable(s) on disk ("
                   << engine.discoveredFilterCount() << " with a loaded bloom filter).\n";
    }
    if (engine.recoveredRecordCount() > 0) {
        std::cout << "Recovered " << engine.recoveredRecordCount() << " record(s) from WAL.\n";
    }

    std::cout << "lsmstore -- commands: SET k v | GET k | DEL k | COUNT | EXIT\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        for (auto& c : cmd) c = static_cast<char>(toupper(c));

        if (cmd == "EXIT" || cmd == "QUIT") {
            break;
        } else if (cmd == "SET") {
            std::string key, value;
            iss >> key;
            std::getline(iss, value);
            if (!value.empty() && value[0] == ' ') value.erase(0, 1);
            if (key.empty() || value.empty()) {
                std::cout << "usage: SET <key> <value>\n";
                continue;
            }
            engine.set(key, value);
            std::cout << "OK\n";
        } else if (cmd == "GET") {
            std::string key;
            iss >> key;
            size_t skipped = 0;
            auto result = engine.get(key, skipped);
            std::cout << (result ? *result : "(not found)");
            if (skipped > 0) {
                std::cout << "  [bloom filter skipped " << skipped << " file(s)]";
            }
            std::cout << "\n";
        } else if (cmd == "DEL") {
            std::string key;
            iss >> key;
            engine.remove(key);
            std::cout << "OK\n";
        } else if (cmd == "COUNT") {
            std::cout << engine.memtableEntryCount() << " entries in memtable, "
                       << engine.sstableFileCount() << " SSTable file(s) on disk\n";
        } else {
            std::cout << "unknown command: " << cmd << "\n";
        }
    }

    std::cout << "bye\n";
    return 0;
}
