#include "sstable.hpp"
#include <fstream>
#include <stdexcept>

namespace {

void writeU32(std::ostream& os, uint32_t v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

uint32_t readU32(std::istream& is) {
    uint32_t v = 0;
    is.read(reinterpret_cast<char*>(&v), sizeof(v));
    return v;
}

constexpr char kMagic[4] = {'S', 'S', 'T', 'B'};

} // namespace

void writeSSTable(const std::string& path, const std::vector<SkipList::Entry>& entries) {
    // fstream (not ofstream) so .sync() is available -- same reasoning
    // as the WAL: an SSTable is only trustworthy once it's actually
    // durable on disk, not just sitting in a C++ stream or OS buffer.
    // This matters especially for compaction, which deletes old SSTables
    // right after writing the merged one -- that's only safe if the new
    // file is genuinely durable first, not just handed off to the OS.
    std::fstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        throw std::runtime_error("failed to create SSTable file: " + path);
    }

    // Header: magic + entry count, so a reader knows up front how many
    // records to expect (and a sanity check against a corrupt/truncated
    // file later, once checksums are added).
    file.write(kMagic, sizeof(kMagic));
    writeU32(file, static_cast<uint32_t>(entries.size()));

    for (const auto& e : entries) {
        uint8_t tombstone = e.tombstone ? 1 : 0;
        file.write(reinterpret_cast<const char*>(&tombstone), sizeof(tombstone));
        writeU32(file, static_cast<uint32_t>(e.key.size()));
        writeU32(file, static_cast<uint32_t>(e.value.size()));
        file.write(e.key.data(), static_cast<std::streamsize>(e.key.size()));
        if (!e.value.empty()) {
            file.write(e.value.data(), static_cast<std::streamsize>(e.value.size()));
        }
    }

    file.flush();
#if defined(_WIN32)
    // See the same caveat in wal.cpp -- a fully portable fsync wrapper
    // is left as a follow-up.
#else
    file.sync();
#endif
    file.close();
}

std::optional<std::pair<std::string, bool>> lookupSSTable(const std::string& path, const std::string& key) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        // A missing/unreadable SSTable is treated as "key not found here"
        // rather than a hard error, so a lookup can keep checking older
        // files instead of crashing the whole read path.
        return std::nullopt;
    }

    char magic[4];
    file.read(magic, sizeof(magic));
    if (!file || std::string(magic, 4) != std::string(kMagic, 4)) {
        return std::nullopt; // not a valid SSTable file
    }

    uint32_t entry_count = readU32(file);

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint8_t tombstone = 0;
        file.read(reinterpret_cast<char*>(&tombstone), sizeof(tombstone));
        uint32_t key_len = readU32(file);
        uint32_t val_len = readU32(file);
        if (!file) break; // truncated/corrupt tail -- stop rather than misread

        std::string cur_key(key_len, '\0');
        if (key_len > 0) file.read(&cur_key[0], key_len);

        // Early exit: entries are sorted ascending, so once we've passed
        // the target key alphabetically, it cannot appear later in this
        // file. This is the whole reason the sorted-file format matters.
        if (cur_key > key) {
            return std::nullopt;
        }

        if (cur_key == key) {
            std::string value(val_len, '\0');
            if (val_len > 0) file.read(&value[0], val_len);
            return std::make_pair(value, tombstone != 0);
        }

        // Not a match -- skip over the value bytes without reading them
        // into a string, since we only needed the key for comparison.
        file.seekg(val_len, std::ios::cur);
    }

    return std::nullopt;
}

std::vector<SkipList::Entry> readAllEntries(const std::string& path) {
    std::vector<SkipList::Entry> out;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return out;

    char magic[4];
    file.read(magic, sizeof(magic));
    if (!file || std::string(magic, 4) != std::string(kMagic, 4)) {
        return out; // not a valid SSTable file
    }

    uint32_t entry_count = readU32(file);
    out.reserve(entry_count);

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint8_t tombstone = 0;
        file.read(reinterpret_cast<char*>(&tombstone), sizeof(tombstone));
        uint32_t key_len = readU32(file);
        uint32_t val_len = readU32(file);
        if (!file) break; // truncated/corrupt tail -- stop rather than misread

        std::string key(key_len, '\0');
        if (key_len > 0) file.read(&key[0], key_len);

        std::string value(val_len, '\0');
        if (val_len > 0) file.read(&value[0], val_len);

        out.push_back({key, value, tombstone != 0});
    }

    return out;
}
