// main.cpp
//
// A minimal REPL on top of the current storage engine core:
// WAL (durability) + SkipList (in-memory sorted memtable).
//
// This is deliberately "phase 1": every write lives in memory (rebuilt
// from the WAL on startup) with no SSTable flush or compaction yet.
// That's the next milestone -- see README.md for the roadmap.
//
// Commands:
//   SET <key> <value>
//   GET <key>
//   DEL <key>
//   COUNT
//   EXIT

#include "skiplist.hpp"
#include "wal.hpp"
#include <iostream>
#include <sstream>
#include <chrono>

int main() {
    const std::string wal_path = "lsmstore.wal";

    SkipList memtable;
    WriteAheadLog wal(wal_path);

    // Crash recovery: rebuild memtable state from whatever was durably
    // logged before the last shutdown/crash.
    size_t replayed = 0;
    wal.replay([&](const WalRecord& rec) {
        if (rec.op == WalOp::PUT) {
            memtable.put(rec.key, rec.value);
        } else {
            memtable.remove(rec.key);
        }
        replayed++;
    });
    if (replayed > 0) {
        std::cout << "Recovered " << replayed << " record(s) from WAL.\n";
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
            wal.append({WalOp::PUT, key, value});
            memtable.put(key, value);
            std::cout << "OK\n";
        } else if (cmd == "GET") {
            std::string key;
            iss >> key;
            auto result = memtable.get(key);
            if (!result || result->second /* tombstone */) {
                std::cout << "(not found)\n";
            } else {
                std::cout << result->first << "\n";
            }
        } else if (cmd == "DEL") {
            std::string key;
            iss >> key;
            wal.append({WalOp::DELETE, key, ""});
            memtable.remove(key);
            std::cout << "OK\n";
        } else if (cmd == "COUNT") {
            std::cout << memtable.approxEntryCount() << " entries\n";
        } else {
            std::cout << "unknown command: " << cmd << "\n";
        }
    }

    std::cout << "bye\n";
    return 0;
}
