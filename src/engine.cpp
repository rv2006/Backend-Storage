#include "engine.hpp"
#include "sstable.hpp"
#include "compaction.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// Kept small on purpose so flushing/compaction are easy to trigger and
// observe by hand in the REPL. A real engine would size the flush
// threshold in MB, not entry count.
namespace {
constexpr size_t FLUSH_THRESHOLD = 5;
constexpr size_t COMPACTION_THRESHOLD = 4;

const std::string kWalPath = "lsmstore.wal";
const std::string kSSTablePrefix = "sstable_";
const std::string kSSTableSuffix = ".dat";
const std::string kFilterSuffix = ".filter";

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

std::string filterPath(const std::string& sstable_path) {
    return sstable_path.substr(0, sstable_path.size() - kSSTableSuffix.size()) + kFilterSuffix;
}

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

} // namespace

Engine::Engine() : memtable_(std::make_unique<SkipList>()), wal_(kWalPath) {
    sstables_ = discoverSSTables(next_sstable_id_);
    discovered_sstable_count_ = sstables_.size();

    for (const auto& path : sstables_) {
        auto loaded = BloomFilter::loadFromFile(filterPath(path));
        if (loaded) {
            filters_.emplace(path, std::move(*loaded));
        }
    }
    discovered_filter_count_ = filters_.size();

    wal_.replay([&](const WalRecord& rec) {
        if (rec.op == WalOp::PUT) {
            memtable_->put(rec.key, rec.value);
        } else {
            memtable_->remove(rec.key);
        }
        recovered_count_++;
    });
}

void Engine::flushMemtable() {
    std::string path = sstablePath(next_sstable_id_++);
    auto entries = memtable_->entriesInOrder();
    writeSSTable(path, entries);

    BloomFilter filter(entries.size());
    for (const auto& e : entries) {
        filter.add(e.key);
    }
    filter.saveToFile(filterPath(path));
    filters_.emplace(path, std::move(filter));

    sstables_.insert(sstables_.begin(), path); // newest first
    memtable_ = std::make_unique<SkipList>();
    wal_.reset(); // WAL only needs to cover data not yet in an SSTable

    if (on_flush) on_flush(path);
}

void Engine::compactIfNeeded() {
    if (sstables_.size() < COMPACTION_THRESHOLD) return;

    size_t files_before = sstables_.size();
    CompactionResult merged = mergeSSTables(sstables_);

    std::string new_path = sstablePath(next_sstable_id_++);
    writeSSTable(new_path, merged.entries);

    BloomFilter filter(merged.entries.size());
    for (const auto& e : merged.entries) {
        filter.add(e.key);
    }
    filter.saveToFile(filterPath(new_path));

    // Only now, after the merged file and its filter are safely on disk,
    // remove the old files and their filter sidecars. Doing the delete
    // last means a crash mid-compaction leaves the old (still-correct)
    // SSTables in place rather than losing data.
    for (const auto& old_path : sstables_) {
        std::error_code ec;
        fs::remove(old_path, ec);
        fs::remove(filterPath(old_path), ec);
        filters_.erase(old_path);
    }

    sstables_.clear();
    sstables_.push_back(new_path);
    filters_.emplace(new_path, std::move(filter));

    if (on_compaction) {
        on_compaction(files_before, new_path, merged.input_entry_count,
                      merged.entries.size(), merged.tombstones_dropped);
    }
}

void Engine::set(const std::string& key, const std::string& value) {
    wal_.append({WalOp::PUT, key, value});
    memtable_->put(key, value);
    if (memtable_->approxEntryCount() >= FLUSH_THRESHOLD) {
        flushMemtable();
        compactIfNeeded();
    }
}

void Engine::remove(const std::string& key) {
    wal_.append({WalOp::DELETE, key, ""});
    memtable_->remove(key);
    if (memtable_->approxEntryCount() >= FLUSH_THRESHOLD) {
        flushMemtable();
        compactIfNeeded();
    }
}

std::optional<std::string> Engine::get(const std::string& key) {
    size_t unused;
    return get(key, unused);
}

std::optional<std::string> Engine::get(const std::string& key, size_t& skipped_via_filter) {
    skipped_via_filter = 0;

    // Check the memtable first -- it always holds the most recent
    // writes, so it must win over anything on disk.
    auto result = memtable_->get(key);
    if (!result) {
        // Not in memory. Fall back to SSTables, newest to oldest, and
        // stop at the first file that has *any* record for this key
        // (live or tombstoned) -- that's the most recent truth for it.
        for (const auto& path : sstables_) {
            auto it = filters_.find(path);
            if (it != filters_.end() && !it->second.mightContain(key)) {
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
        return std::nullopt;
    }
    return result->first;
}

size_t Engine::memtableEntryCount() const {
    return memtable_->approxEntryCount();
}

size_t Engine::sstableFileCount() const {
    return sstables_.size();
}
