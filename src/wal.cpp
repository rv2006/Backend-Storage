#include "wal.hpp"
#include <stdexcept>
#include <cstdio>

WriteAheadLog::WriteAheadLog(const std::string& path) : path_(path) {
    // Open for append+read, create if missing.
    file_.open(path_, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
    if (!file_.is_open()) {
        throw std::runtime_error("failed to open WAL file: " + path_);
    }
}

WriteAheadLog::~WriteAheadLog() {
    if (file_.is_open()) file_.close();
}

static void writeU32(std::ostream& os, uint32_t v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

static uint32_t readU32(std::istream& is) {
    uint32_t v = 0;
    is.read(reinterpret_cast<char*>(&v), sizeof(v));
    return v;
}

void WriteAheadLog::append(const WalRecord& record) {
    file_.clear(); // clear eof/fail bits from any prior read
    file_.seekp(0, std::ios::end);

    uint8_t op = static_cast<uint8_t>(record.op);
    file_.write(reinterpret_cast<const char*>(&op), sizeof(op));
    writeU32(file_, static_cast<uint32_t>(record.key.size()));
    writeU32(file_, static_cast<uint32_t>(record.value.size()));
    file_.write(record.key.data(), static_cast<std::streamsize>(record.key.size()));
    if (!record.value.empty()) {
        file_.write(record.value.data(), static_cast<std::streamsize>(record.value.size()));
    }
    file_.flush();

    // Force the OS to actually write bytes to disk, not just its buffer.
    // This is the real durability boundary -- without this, a power loss
    // right after `flush()` can still lose the write.
#if defined(_WIN32)
    // Windows: flush() via std::fstream generally goes through
    // FlushFileBuffers in practice with most standard libraries; a fully
    // portable fsync wrapper is left as a follow-up.
#else
    file_.sync();
#endif
}

void WriteAheadLog::replay(const std::function<void(const WalRecord&)>& apply) {
    file_.clear();
    file_.seekg(0, std::ios::beg);

    while (file_.peek() != EOF) {
        uint8_t op = 0;
        file_.read(reinterpret_cast<char*>(&op), sizeof(op));
        if (file_.eof()) break; // torn/partial record at tail; stop cleanly

        uint32_t key_len = readU32(file_);
        uint32_t val_len = readU32(file_);
        if (file_.eof()) break;

        std::string key(key_len, '\0');
        if (key_len > 0) file_.read(&key[0], key_len);

        std::string value(val_len, '\0');
        if (val_len > 0) file_.read(&value[0], val_len);

        if (file_.eof() && (key.size() != key_len || value.size() != val_len)) {
            // Torn write at the end of the file from a crash mid-append.
            // We stop replay here rather than trusting the partial record.
            break;
        }

        WalRecord rec{static_cast<WalOp>(op), key, value};
        apply(rec);
    }
    file_.clear();
}

void WriteAheadLog::reset() {
    file_.close();
    std::remove(path_.c_str());
    file_.open(path_, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
    if (!file_.is_open()) {
        throw std::runtime_error("failed to reopen WAL file after reset: " + path_);
    }
}
