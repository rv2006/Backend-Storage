#pragma once
// wal.hpp
//
// Write-Ahead Log.
//
// The rule that makes this a real database and not a toy: never apply a
// write to the in-memory memtable until it has been durably recorded here
// first. If the process crashes between "recorded in WAL" and "applied to
// memtable", we recover by replaying the WAL on startup. If it crashes
// before the WAL write completes, the write never happened as far as any
// client can tell (as long as we call fsync at the right point) — that's
// the durability guarantee.
//
// On-disk record format (all integers little-endian, fixed-width so
// parsing never has to guess where a field ends):
//
//   [1 byte]  op         0 = PUT, 1 = DELETE
//   [4 bytes] key_len    uint32
//   [4 bytes] val_len    uint32 (0 for DELETE)
//   [key_len bytes]  key
//   [val_len bytes]  value
//
// This is intentionally simple (no checksums yet). A natural next step,
// once this works end to end, is adding a CRC32 per record so a torn
// write at the tail of the file (from a crash mid-write) can be detected
// and truncated during replay instead of corrupting recovery.

#include <string>
#include <vector>
#include <fstream>
#include <functional>
#include <cstdint>

enum class WalOp : uint8_t { PUT = 0, DELETE = 1 };

struct WalRecord {
    WalOp op;
    std::string key;
    std::string value; // empty for DELETE
};

class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& path);
    ~WriteAheadLog();

    // Appends one record and fsyncs before returning. This is the
    // durability boundary: once this returns, the write survives a crash.
    void append(const WalRecord& record);

    // Replays every record in the log file, invoking `apply` for each one
    // in the order they were written. Used on startup to rebuild the
    // memtable after a restart/crash.
    void replay(const std::function<void(const WalRecord&)>& apply);

    // Wipes the log (called after a successful memtable -> SSTable flush,
    // since the WAL only needs to cover data not yet persisted as an
    // SSTable).
    void reset();

private:
    std::string path_;
    std::fstream file_;
};
