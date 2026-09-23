#pragma once
// bloomfilter.hpp
//
// A bloom filter: a fixed-size bit array plus a handful of hash
// functions, built once per SSTable when it's written.
//
// The guarantee it gives you:
//   - mightContain(key) == false  ->  key is DEFINITELY NOT in the file.
//     Skip the file entirely, no disk read needed.
//   - mightContain(key) == true   ->  key MIGHT be in the file (or this is
//     a false positive). You still have to actually scan the file to know.
//
// It never produces a false negative -- if the key really is in the
// SSTable, mightContain() is guaranteed to return true for it. The only
// error it can make is a false positive (saying "maybe" for a key that
// isn't actually there), and the false-positive rate is tunable by
// choosing more bits / more hash functions, at the cost of a bigger
// filter.
//
// Why this matters for an LSM-tree: without this, every GET that misses
// the memtable has to open and scan every single SSTable on disk (even
// with the early-exit optimization already in sstable.cpp, that's still
// real disk I/O per file). A bloom filter turns most of those "the key
// isn't in this old file" cases into a few nanoseconds of in-memory bit
// checks instead of a disk read.
//
// How the hashing works: rather than implementing k independent hash
// functions from scratch, this uses the standard "double hashing" trick
// -- compute two independent hashes (h1, h2) of the key, then derive k
// hash values as h1 + i*h2 for i in [0, k). This is a well-known,
// good-enough approximation of k independent hashes for bloom filter
// purposes, and it's what most real implementations do rather than
// writing out k separate hash functions.

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <optional>

class BloomFilter {
public:
    // num_entries: how many keys you expect to insert (usually the
    //   SSTable's entry count).
    // bits_per_entry: size/accuracy knob. 10 bits/entry gives roughly a
    //   1% false-positive rate, which is the standard rule-of-thumb
    //   default real bloom filters (e.g. RocksDB's) tend to use.
    explicit BloomFilter(size_t num_entries, size_t bits_per_entry = 10);

    // Reconstructs a filter from raw bits read off disk (used when
    // loading a previously-written filter file). num_bits must be the
    // exact value used when the filter was built (not just bits.size()*8,
    // which is padded up to a byte boundary and would silently shift
    // every hash index, turning real entries into false negatives).
    // num_hashes must also match what was used originally.
    BloomFilter(std::vector<uint8_t> bits, size_t num_bits, int num_hashes);

    void add(const std::string& key);

    // false = key is definitely not present.
    // true  = key might be present (verify with an actual scan).
    bool mightContain(const std::string& key) const;

    // Persists the filter's bit array + hash count to `path`, so it can
    // be loaded back on the next startup instead of rebuilt from scratch.
    void saveToFile(const std::string& path) const;

    // Loads a filter previously written by saveToFile(). Returns
    // std::nullopt if the file doesn't exist or is unreadable -- callers
    // should treat that as "no filter available, fall back to always
    // scanning the SSTable" rather than as a hard error.
    static std::optional<BloomFilter> loadFromFile(const std::string& path);

private:
    std::vector<uint8_t> bits_; // one bit per entry, packed 8 per byte
    size_t num_bits_;
    int num_hashes_;

    void setBit(size_t index);
    bool getBit(size_t index) const;

    // Computes two independent 64-bit hashes of `key`, used as the seeds
    // for the double-hashing scheme described above.
    static void hashKey(const std::string& key, uint64_t& h1, uint64_t& h2);
};
