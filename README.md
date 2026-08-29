# lsmstore

A key-value storage engine built from scratch in C++ — the same class of
system that powers the storage layer inside RocksDB, LevelDB, and
Cassandra. This is not a wrapper around a database; it *is* the database
(the part below the query layer).

## Why this project exists

Most student projects use a database. This one builds the thing databases
are built on: how writes get made durable, how data gets kept sorted
without paying the cost of sorting on every write, and how a background
process cleans up after itself so reads stay fast over time.

## Current status: Phase 1 (durable in-memory store)

What's implemented and working right now:

- **Write-Ahead Log (`wal.hpp` / `wal.cpp`)** — every write is appended to
  an on-disk log and `fsync`'d *before* it's applied to memory. This is
  the durability boundary: if the process crashes right after a write
  returns "OK", that write is guaranteed to survive.
- **Skip List memtable (`skiplist.hpp`)** — the in-memory sorted
  structure that serves reads and writes. Deletes are tombstoned rather
  than physically removed, since a future SSTable layer needs that
  history to correctly shadow older on-disk values.
- **Crash recovery** — on startup, the WAL is replayed in order to
  rebuild the memtable exactly as it was before shutdown/crash. Verified
  by killing the process mid-session and confirming all writes survive
  restart.
- **CLI (`main.cpp`)** — a REPL for `SET`, `GET`, `DEL`, `COUNT`.

Everything currently lives in memory (rebuilt from the WAL on every
start). That's intentional — it's the honest, fully-working "phase 1"
rather than a half-built version of the whole thing.

## Roadmap (in build order)

1. **SSTable flush** — once the memtable crosses a size threshold, write
   its sorted contents to an immutable on-disk file (Sorted String
   Table) and start a fresh memtable + WAL. This is what lets the engine
   hold more data than fits in RAM.
2. **Multi-file reads** — `GET` needs to check the memtable, then scan
   SSTables newest-to-oldest until it finds the key (or exhausts them).
3. **Bloom filters** — one per SSTable, to skip files that provably don't
   contain a key instead of reading them off disk.
4. **Compaction** — background merging of old SSTables into fewer,
   cleaner files, dropping tombstoned/superseded entries. This is the
   hardest and most interesting part of the whole system.
5. **Benchmarking** — throughput numbers (writes/sec, reads/sec) using
   Google Benchmark, and a short write-up of the write/read/space
   amplification tradeoffs this design makes.

## Building

Requires a C++17 compiler. No external dependencies for the core engine.

```bash
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude src/main.cpp src/wal.cpp -o lsmstore
./lsmstore
```

Or with CMake:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
./lsmstore
```

## Usage

```
> SET name viyhash
OK
> GET name
viyhash
> DEL name
OK
> GET name
(not found)
> COUNT
0 entries
> EXIT
```

Kill the process (Ctrl+C) mid-session and restart — every write that
returned "OK" will still be there, replayed from the WAL.

## Design notes / talking points

- **On-disk WAL record format** is fixed-width and hand-rolled (no
  serialization library): `[1 byte op][4 byte key_len][4 byte val_len]
  [key bytes][value bytes]`. No checksums yet — a natural hardening step
  is adding a CRC32 per record so a torn write at the tail (from a crash
  mid-append) can be detected and the log truncated cleanly during
  replay instead of risking corrupt recovery.
- **Why tombstones instead of physical delete**: once data can live
  across multiple immutable SSTable files, a `DELETE` has to be able to
  "shadow" a value that already exists in an older file. Physically
  removing the key from the memtable would lose that information and a
  stale value could resurface from an older SSTable on read.
- **Why a skip list over `std::map`**: comparable O(log n) average-case
  performance, but a much simpler mental model, and it extends more
  naturally to lock-free / fine-grained-locking concurrent versions
  later, which real memtables need.
