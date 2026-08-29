#pragma once
// skiplist.hpp
//
// A skip list mapping std::string -> std::string.
// This is the in-memory data structure behind the memtable.
//
// Why a skip list instead of std::map (a red-black tree)?
//   - Simpler to reason about under concurrent access (this starter version
//     is single-threaded; a lock-free/finer-grained version is a natural
//     "phase 2" upgrade once the basics work).
//   - O(log n) expected search/insert/delete, same asymptotic class as a
//     balanced tree, but the implementation is much shorter and easier to
//     get right, which matters a lot when this file is the thing you're
//     going to be quizzed on in an interview.
//
// Deletions are represented as tombstones (a special "deleted" marker),
// not physically removed. This matters later: once we have SSTables on
// disk, a delete has to be able to "shadow" an older value that already
// lives in an older, immutable file. Physically removing here would lose
// that information.

#include <string>
#include <vector>
#include <random>
#include <optional>
#include <cstdint>

class SkipList {
public:
    struct Entry {
        std::string key;
        std::string value;
        bool tombstone; // true = this is a delete marker
    };

    explicit SkipList(int max_level = 16, float p = 0.5f)
        : max_level_(max_level), p_(p), level_(1), rng_(std::random_device{}()) {
        head_ = new Node("", "", false, max_level_);
    }

    ~SkipList() {
        Node* node = head_;
        while (node) {
            Node* next = node->forward[0];
            delete node;
            node = next;
        }
    }

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Insert or overwrite a live value for `key`.
    void put(const std::string& key, const std::string& value) {
        upsert(key, value, /*tombstone=*/false);
    }

    // Mark `key` as deleted (tombstone), without removing prior history.
    void remove(const std::string& key) {
        upsert(key, "", /*tombstone=*/true);
    }

    // Returns:
    //   {value, false} if key exists with a live value
    //   {"", true}     if key exists but is tombstoned (i.e. was deleted)
    //   std::nullopt    if key was never seen at all in this skip list
    std::optional<std::pair<std::string, bool>> get(const std::string& key) const {
        Node* node = findNode(key);
        if (node && node->key == key) {
            return std::make_pair(node->value, node->tombstone);
        }
        return std::nullopt;
    }

    // Number of live entries (rough size accounting; used to decide when
    // the memtable should be flushed to an SSTable).
    size_t approxEntryCount() const { return count_; }

    // In-order iteration, needed later for flushing to a sorted SSTable file.
    std::vector<Entry> entriesInOrder() const {
        std::vector<Entry> out;
        out.reserve(count_);
        Node* node = head_->forward[0];
        while (node) {
            out.push_back({node->key, node->value, node->tombstone});
            node = node->forward[0];
        }
        return out;
    }

private:
    struct Node {
        std::string key;
        std::string value;
        bool tombstone;
        std::vector<Node*> forward;

        Node(std::string k, std::string v, bool t, int level)
            : key(std::move(k)), value(std::move(v)), tombstone(t), forward(level, nullptr) {}
    };

    Node* head_;
    int max_level_;
    float p_;
    int level_;
    size_t count_ = 0;
    mutable std::mt19937 rng_;

    int randomLevel() {
        int lvl = 1;
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        while (dist(rng_) < p_ && lvl < max_level_) {
            lvl++;
        }
        return lvl;
    }

    Node* findNode(const std::string& key) const {
        Node* cur = head_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (cur->forward[i] && cur->forward[i]->key < key) {
                cur = cur->forward[i];
            }
        }
        cur = cur->forward[0];
        return cur;
    }

    void upsert(const std::string& key, const std::string& value, bool tombstone) {
        std::vector<Node*> update(max_level_, head_);
        Node* cur = head_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (cur->forward[i] && cur->forward[i]->key < key) {
                cur = cur->forward[i];
            }
            update[i] = cur;
        }
        cur = cur->forward[0];

        if (cur && cur->key == key) {
            // Overwrite in place.
            cur->value = value;
            cur->tombstone = tombstone;
            return;
        }

        int new_level = randomLevel();
        if (new_level > level_) {
            for (int i = level_; i < new_level; ++i) {
                update[i] = head_;
            }
            level_ = new_level;
        }

        Node* node = new Node(key, value, tombstone, new_level);
        for (int i = 0; i < new_level; ++i) {
            node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = node;
        }
        count_++;
    }
};
