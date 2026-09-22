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

## Current status: Phase 2 (SSTable flush)

What's implemented and working right now:

- **Write-Ahead Log (`wal.hpp` / `wal.cpp`)** — every write is appended to
  an on-disk log and `fsync`'d *before* it's applied to memory. This is
  the durability boundary: if the process crashes right after a write
  returns "OK", that write is guaranteed to survive.
- **Skip List memtable (`skiplist.hpp`)** — the in-memory sorted
  structure that serves reads and writes. Deletes are tombstoned rather
  than physically removed, since SSTables need that history to correctly
  shadow older on-disk values.
- **SSTable flush (`sstable.hpp` / `sstable.cpp`)** — once the memtable
  crosses a size threshold, its sorted contents are written to an
  immutable, sorted file on disk (a Sorted String Table), and a fresh
  memtable + WAL take over. This is what makes the engine an actual
  LSM-tree rather than just a durable in-memory store: data can now
  exceed what fits in RAM.
- **Multi-file reads** — `GET` checks the memtable first (it always holds
  the most recent writes), then falls back to scanning SSTables
  newest-to-oldest, stopping at the first file that has any record
  (live or tombstoned) for the key. Verified: a key updated across two
  separate flushes correctly resolves to the value in the newer file.
- **Crash recovery, extended** — on startup, the engine now rediscovers
  existing SSTable files on disk (by scanning for `sstable_NNNN.dat`)
  *and* replays the WAL to rebuild whatever memtable state hadn't been
  flushed yet. Verified across a restart with both an on-disk SSTable and
  unflushed WAL data present simultaneously.
- **CLI (`main.cpp`)** — a REPL for `SET`, `GET`, `DEL`, `COUNT`.

## Roadmap (in build order)

1. ~~**SSTable flush**~~ — done.
2. ~~**Multi-file reads**~~ — done.
3. **Bloom filters** — one per SSTable, to skip files that provably don't
   contain a key instead of reading them off disk. Right now every GET
   that misses the memtable does a linear scan (with early exit) through
   every SSTable file — this is the next real inefficiency to fix.
4. **Compaction** — background merging of old SSTables into fewer,
   cleaner files, dropping tombstoned/superseded entries. This is the
   hardest and most interesting part of the whole system. Right now
   SSTables just pile up forever with no cleanup.
5. **Benchmarking** — throughput numbers (writes/sec, reads/sec) using
   Google Benchmark, and a short write-up of the write/read/space
   amplification tradeoffs this design makes.

## Building

Requires a C++17 compiler. No external dependencies for the core engine.

```bash
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude src/main.cpp src/wal.cpp src/sstable.cpp -o lsmstore
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
0 entries in memtable, 0 SSTable file(s) on disk
> EXIT
```

The flush threshold is deliberately small (5 entries — see
`FLUSH_THRESHOLD` in `main.cpp`) so you can trigger and observe a flush
by hand in the REPL instead of needing thousands of writes:

```
> SET a 1
OK
> SET b 2
OK
> SET c 3
OK
> SET d 4
OK
> SET e 5
OK
(flushed memtable -> sstable_0000.dat)
> GET a
1
```
`GET a` here is served entirely from the SSTable on disk — the memtable
was just reset to empty by the flush.

Kill the process (Ctrl+C) mid-session and restart — every write that
returned "OK" will still be there: flushed data comes back from the
rediscovered SSTable files, unflushed data comes back from the WAL.

## Design notes / talking points

- **On-disk WAL and SSTable record formats** are fixed-width and
  hand-rolled (no serialization library). No checksums yet — a natural
  hardening step is adding a CRC32 per record so a torn write at the tail
  (from a crash mid-append) can be detected and the log/file truncated
  cleanly during replay/read instead of risking corrupt recovery.
- **Why tombstones instead of physical delete**: once data lives across
  multiple immutable SSTable files, a `DELETE` has to be able to "shadow"
  a value that already exists in an older file. Physically removing the
  key from the memtable — or from an SSTable, which can't be edited at
  all — would lose that information and a stale value could resurface on
  read.
- **Why SSTables are immutable**: once written, a file is never edited in
  place. An update to an existing key is handled by writing a *newer*
  record (in the memtable, then a newer SSTable) that shadows the old
  one on read, rather than mutating the old file. This is what makes
  compaction (Phase 4) a distinct, separable step instead of something
  that has to happen synchronously on every write.
- **Why point lookups do a linear scan with early exit, for now**: SSTable
  entries are sorted, so a lookup can stop scanning the moment it passes
  the target key alphabetically — it doesn't need to read the rest of the
  file. This is still O(n) in the worst case though; a bloom filter
  (Phase 3) is what lets a lookup skip opening a file entirely when the
  key provably isn't in it.
- **Why a skip list over `std::map`**: comparable O(log n) average-case
  performance, but a much simpler mental model, and it extends more
  naturally to lock-free / fine-grained-locking concurrent versions
  later, which real memtables need.
