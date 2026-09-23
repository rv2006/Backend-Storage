// main.cpp
//
// A REPL on top of the storage engine core: WAL (durability) + SkipList
// (in-memory memtable) + SSTable (on-disk flush) + BloomFilter (skip
// SSTables that provably don't contain a key).
//
// This is "phase 3": every SSTable now gets a bloom filter built and
// saved alongside it when it's flushed. GET checks the filter before
// scanning a file -- a "definitely not present" answer skips the disk
// read entirely instead of opening the file and scanning it.
//
// Still missing (see README roadmap): compaction (to merge/clean up old
// SSTables instead of letting them pile up forever).
//
// Commands:
//   SET <key> <value>
//   GET <key>
//   DEL <key>
//   COUNT
//   EXIT

#include "skiplist.hpp"
#include "wal.hpp"
#include "sstable.hpp"
#include "bloomfilter.hpp"
#include <iostream>
#include <sstream>
#include <memory>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <iomanip>

namespace fs = std::filesystem;

// Kept small on purpose so flushing is easy to trigger and observe by
// hand in the REPL. A real engine would size this in MB, not entry count.
constexpr size_t FLUSH_THRESHOLD = 5;

const std::string kSSTablePrefix = "sstable_";
const std::string kSSTableSuffix = ".dat";
const std::string kFilterSuffix = ".filter";

// Parses the numeric id out of "sstable_0003.dat" -> 3. Returns -1 if the
// filename doesn't match the expected pattern.
int parseSSTableId(const std::string& filename) {
    if (filename.rfind(kSSTablePrefix, 0) != 0) return -1;
    if (filename.size() < kSSTablePrefix.size() + kSSTableSuffix.size()) return -1;
    std::string mid = filename.substr(
        kSSTablePrefix.size(),
        filename.size() - kSSTablePrefix.size() - kSSTableSuffix.size());
    if (mid.empty() || !std::all_of(mid.begin(), mid.end(), ::isdigit)) return -1;
    return std::stoi(mid);
}

std::string sstablePath(int id) {
    std::ostringstream oss;
    oss << kSSTablePrefix << std::setw(4) << std::setfill('0') << id << kSSTableSuffix;
    return oss.str();
}

// A bloom filter's file always sits next to its SSTable with the same
// stem: "sstable_0003.dat" -> "sstable_0003.filter".
std::string filterPath(const std::string& sstable_path) {
    return sstable_path.substr(0, sstable_path.size() - kSSTableSuffix.size()) + kFilterSuffix;
}

// Scans the working directory for existing SSTable files on startup, so
// a restart picks up where the engine left off -- not just the WAL, but
// on-disk SSTables from previous flushes too.
// Returns paths sorted newest-first (highest id first), which is the
// order GET needs to search in.
std::vector<std::string> discoverSSTables(int& next_id_out) {
    std::vector<std::pair<int, std::string>> found;
    for (const auto& entry : fs::directory_iterator(fs::current_path())) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        int id = parseSSTableId(name);
        if (id >= 0) found.push_back({id, name});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first > b.first; // newest (highest id) first
    });

    std::vector<std::string> paths;
    int max_id = -1;
    for (const auto& [id, name] : found) {
        paths.push_back(name);
        max_id = std::max(max_id, id);
    }
    next_id_out = max_id + 1;
    return paths;
}

int main() {
    const std::string wal_path = "lsmstore.wal";

    auto memtable = std::make_unique<SkipList>();
    WriteAheadLog wal(wal_path);

    int next_sstable_id = 0;
    std::vector<std::string> sstables = discoverSSTables(next_sstable_id);

    // One bloom filter per SSTable, keyed by the SSTable's path. Loaded
    // from its sidecar .filter file on startup; built fresh at flush
    // time. If a filter is missing for some file (e.g. an older SSTable
    // from before this feature existed), GET just always scans that file
    // -- a missing filter is treated as "no shortcut available", not an
    // error.
    std::unordered_map<std::string, BloomFilter> filters;
    for (const auto& path : sstables) {
        auto loaded = BloomFilter::loadFromFile(filterPath(path));
        if (loaded) {
            filters.emplace(path, std::move(*loaded));
        }
    }
    if (!sstables.empty()) {
        std::cout << "Found " << sstables.size() << " existing SSTable(s) on disk ("
                   << filters.size() << " with a loaded bloom filter).\n";
    }

    // Crash recovery: rebuild memtable state from whatever was durably
    // logged (but not yet flushed to an SSTable) before the last
    // shutdown/crash.
    size_t replayed = 0;
    wal.replay([&](const WalRecord& rec) {
        if (rec.op == WalOp::PUT) {
            memtable->put(rec.key, rec.value);
        } else {
            memtable->remove(rec.key);
        }
        replayed++;
    });
    if (replayed > 0) {
        std::cout << "Recovered " << replayed << " record(s) from WAL.\n";
    }

    std::cout << "lsmstore -- commands: SET k v | GET k | DEL k | COUNT | EXIT\n";

    auto flushMemtable = [&]() {
        std::string path = sstablePath(next_sstable_id++);
        auto entries = memtable->entriesInOrder();
        writeSSTable(path, entries);

        // Build the filter from the same entries just written, save it
        // alongside the SSTable, and keep it in memory so this session
        // benefits from it immediately (not just after a restart).
        BloomFilter filter(entries.size());
        for (const auto& e : entries) {
            filter.add(e.key);
        }
        filter.saveToFile(filterPath(path));
        filters.emplace(path, std::move(filter));

        sstables.insert(sstables.begin(), path); // newest first
        memtable = std::make_unique<SkipList>();
        wal.reset(); // WAL only needs to cover data not yet in an SSTable
        std::cout << "(flushed memtable -> " << path << ", bloom filter written)\n";
    };

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
            wal.append({WalOp::PUT, key, value});
            memtable->put(key, value);
            std::cout << "OK\n";
            if (memtable->approxEntryCount() >= FLUSH_THRESHOLD) {
                flushMemtable();
            }
        } else if (cmd == "GET") {
            std::string key;
            iss >> key;

            // Check the memtable first -- it always holds the most recent
            // writes, so it must win over anything on disk.
            auto result = memtable->get(key);
            size_t skipped_via_filter = 0;
            if (!result) {
                // Not in memory. Fall back to SSTables, newest to oldest,
                // and stop at the first file that has *any* record for
                // this key (live or tombstoned) -- that's the most recent
                // truth for it.
                for (const auto& path : sstables) {
                    auto it = filters.find(path);
                    if (it != filters.end() && !it->second.mightContain(key)) {
                        // Bloom filter guarantees the key isn't in this
                        // file -- skip opening/scanning it entirely.
                        skipped_via_filter++;
                        continue;
                    }
                    auto sstable_result = lookupSSTable(path, key);
                    if (sstable_result) {
                        result = sstable_result;
                        break;
                    }
                }
            }

            if (!result || result->second /* tombstone */) {
                std::cout << "(not found)";
            } else {
                std::cout << result->first;
            }
            if (skipped_via_filter > 0) {
                std::cout << "  [bloom filter skipped " << skipped_via_filter << " file(s)]";
            }
            std::cout << "\n";
        } else if (cmd == "DEL") {
            std::string key;
            iss >> key;
            wal.append({WalOp::DELETE, key, ""});
            memtable->remove(key);
            std::cout << "OK\n";
            if (memtable->approxEntryCount() >= FLUSH_THRESHOLD) {
                flushMemtable();
            }
        } else if (cmd == "COUNT") {
            std::cout << memtable->approxEntryCount() << " entries in memtable, "
                       << sstables.size() << " SSTable file(s) on disk\n";
        } else {
            std::cout << "unknown command: " << cmd << "\n";
        }
    }

    std::cout << "bye\n";
    return 0;
}

