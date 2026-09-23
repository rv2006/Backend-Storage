#include "bloomfilter.hpp"
#include <cmath>
#include <functional>

BloomFilter::BloomFilter(size_t num_entries, size_t bits_per_entry) {
    // Guard against a degenerate empty SSTable (0 entries) still needing
    // a valid, non-zero-sized filter.
    size_t entries = num_entries > 0 ? num_entries : 1;
    num_bits_ = entries * bits_per_entry;

    // Standard formula for the optimal number of hash functions given a
    // bits-per-entry budget: k = (bits/entry) * ln(2). Rounding to the
    // nearest integer, with a floor of 1.
    num_hashes_ = static_cast<int>(std::round(bits_per_entry * 0.6931471805599453));
    if (num_hashes_ < 1) num_hashes_ = 1;

    bits_.assign((num_bits_ + 7) / 8, 0);
}

BloomFilter::BloomFilter(std::vector<uint8_t> bits, size_t num_bits, int num_hashes)
    : bits_(std::move(bits)), num_bits_(num_bits), num_hashes_(num_hashes) {}

void BloomFilter::setBit(size_t index) {
    bits_[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
}

bool BloomFilter::getBit(size_t index) const {
    return (bits_[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

void BloomFilter::hashKey(const std::string& key, uint64_t& h1, uint64_t& h2) {
    // h1: the standard library's string hash -- good general-purpose
    // distribution, treated as our first independent hash.
    h1 = std::hash<std::string>{}(key);

    // h2: a separate hash via FNV-1a, hand-rolled so it's a genuinely
    // different function from h1 rather than a derivative of it (using
    // two correlated hashes would make the double-hashing trick below
    // behave like a single weaker hash instead of two independent ones).
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ULL; // FNV prime
    }
    h2 = hash;
}

void BloomFilter::add(const std::string& key) {
    uint64_t h1, h2;
    hashKey(key, h1, h2);
    for (int i = 0; i < num_hashes_; ++i) {
        size_t idx = static_cast<size_t>((h1 + static_cast<uint64_t>(i) * h2) % num_bits_);
        setBit(idx);
    }
}

bool BloomFilter::mightContain(const std::string& key) const {
    uint64_t h1, h2;
    hashKey(key, h1, h2);
    for (int i = 0; i < num_hashes_; ++i) {
        size_t idx = static_cast<size_t>((h1 + static_cast<uint64_t>(i) * h2) % num_bits_);
        if (!getBit(idx)) {
            // A single unset bit is proof the key was never added --
            // this is the whole mechanism that makes "definitely not
            // present" a guarantee rather than a guess.
            return false;
        }
    }
    return true; // every relevant bit was set -- maybe present
}

void BloomFilter::saveToFile(const std::string& path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return; // best-effort: a missing filter file just means "always scan" later

    uint32_t num_bits32 = static_cast<uint32_t>(num_bits_);
    int32_t num_hashes32 = static_cast<int32_t>(num_hashes_);
    uint32_t byte_count = static_cast<uint32_t>(bits_.size());

    file.write(reinterpret_cast<const char*>(&num_bits32), sizeof(num_bits32));
    file.write(reinterpret_cast<const char*>(&num_hashes32), sizeof(num_hashes32));
    file.write(reinterpret_cast<const char*>(&byte_count), sizeof(byte_count));
    file.write(reinterpret_cast<const char*>(bits_.data()), static_cast<std::streamsize>(bits_.size()));
}

std::optional<BloomFilter> BloomFilter::loadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    uint32_t num_bits32 = 0;
    int32_t num_hashes32 = 0;
    uint32_t byte_count = 0;
    file.read(reinterpret_cast<char*>(&num_bits32), sizeof(num_bits32));
    file.read(reinterpret_cast<char*>(&num_hashes32), sizeof(num_hashes32));
    file.read(reinterpret_cast<char*>(&byte_count), sizeof(byte_count));
    if (!file) return std::nullopt;

    std::vector<uint8_t> bits(byte_count);
    if (byte_count > 0) {
        file.read(reinterpret_cast<char*>(bits.data()), byte_count);
        if (!file) return std::nullopt;
    }

    return BloomFilter(std::move(bits), num_bits32, num_hashes32);
}
