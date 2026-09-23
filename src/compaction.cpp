#include "compaction.hpp"
#include "sstable.hpp"
#include <unordered_map>
#include <algorithm>

CompactionResult mergeSSTables(const std::vector<std::string>& sstable_paths) {
    CompactionResult result;

    // Newest-version-wins per key. Walking the input files in the same
    // newest-to-oldest order GET already uses means the *first* time we
    // see a given key is automatically its most recent value -- so we
    // just skip any key we've already recorded.
    std::unordered_map<std::string, SkipList::Entry> latest;

    for (const auto& path : sstable_paths) {
        auto entries = readAllEntries(path);
        result.input_entry_count += entries.size();
        for (auto& e : entries) {
            latest.emplace(e.key, std::move(e)); // emplace: no-op if key already present
        }
    }

    result.entries.reserve(latest.size());
    for (auto& [key, entry] : latest) {
        if (entry.tombstone) {
            // Safe to drop: this is a full compaction, so there's no
            // older file left outside the merge for this tombstone to
            // still need to shadow.
            result.tombstones_dropped++;
            continue;
        }
        result.entries.push_back(std::move(entry));
    }

    // SSTable format requires ascending sorted order -- the unordered_map
    // above gave us dedup but not order, so restore it here.
    std::sort(result.entries.begin(), result.entries.end(),
              [](const SkipList::Entry& a, const SkipList::Entry& b) { return a.key < b.key; });

    return result;
}
