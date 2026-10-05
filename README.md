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

## Current status: complete

Every stage of the LSM-tree loop is implemented, tested, and benchmarked:
memtable → WAL → SSTable → bloom filter → compaction, driven through a
reusable `Engine` class that both the REPL and the benchmark harness
share. What's implemented and working right now:

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
- **Bloom filters (`bloomfilter.hpp` / `bloomfilter.cpp`)** — every
  SSTable gets a bloom filter built at flush time and saved alongside it
  as a `.filter` sidecar file. `GET` checks the filter before opening an
  SSTable; a "definitely not present" result skips the file entirely
  instead of scanning it. Filters are reloaded from disk on restart
  rather than rebuilt. Verified: a lookup for a key that exists in an
  older SSTable correctly skips a newer SSTable's file via its filter
  before finding the right value, both in the same session and after a
  restart with filters loaded fresh from disk.
- **Compaction (`compaction.hpp` / `compaction.cpp`)** — once the number
  of on-disk SSTables crosses a threshold, they're all merged into a
  single new SSTable: duplicate keys resolve to their newest value, and
  tombstones are dropped entirely (safe here because this is a *full*
  merge — nothing older is left outside it for a tombstone to still need
  to shadow). The new merged file and its bloom filter are written and
  durably synced to disk *before* the old files are deleted, so a crash
  mid-compaction leaves the still-correct old SSTables in place instead
  of losing data. Verified: 4 SSTables (20 raw entries, including one
  overwritten key and one deleted key) compact down to 17 live entries
  in a single file, the old files are actually removed from disk, and
  every key — including the overwritten and deleted ones — resolves
  correctly both immediately after compaction and after a restart.
- **`Engine` class (`engine.hpp` / `engine.cpp`)** — the WAL + memtable +
  SSTable + bloom filter + compaction logic above, originally written
  inline in `main()`, refactored out into a standalone class with a real
  API (`set`/`remove`/`get`). This is what makes a benchmark possible at
  all: driving the engine by typing commands into a REPL can't exercise
  thousands of operations in a tight timed loop, so the engine needed an
  API a benchmark (or, later, unit tests) could call directly, not just
  stdin. No behavior changed in this refactor — verified by re-running
  the full compaction regression test against the refactored engine and
  getting byte-for-byte identical output to before the refactor.
- **CLI (`main.cpp`)** — a thin REPL (`SET`, `GET`, `DEL`, `COUNT`) over
  `Engine`.
- **Benchmark (`benchmark.cpp`)** — a standalone throughput harness
  against the same `Engine` class the REPL uses. See Benchmark results
  below for real, measured numbers and what they actually show.

## Roadmap

All planned phases are done:

1. ~~**SSTable flush**~~
2. ~~**Multi-file reads**~~
3. ~~**Bloom filters**~~
4. ~~**Compaction**~~ — currently a *full* compaction (all SSTables merged
   in one shot) rather than the tiered/leveled strategies real engines
   use to avoid re-merging the whole dataset every time. A natural next
   hardening step, not required for the core story.
5. ~~**Benchmarking**~~

**What a next iteration would add** (see Benchmark results below for
why): a sparse index per SSTable, so a point lookup on an existing key
can jump close to it instead of linearly scanning the file from the
start. The benchmark is what surfaced this as the real next bottleneck,
not a guess.

## Building

Requires a C++17 compiler. No external dependencies for the core engine.

```bash
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude src/main.cpp src/wal.cpp src/sstable.cpp src/bloomfilter.cpp src/compaction.cpp src/engine.cpp -o lsmstore
./lsmstore
```

Or with CMake, which also builds the benchmark (`lsmstore_bench`):

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
./lsmstore
./lsmstore_bench
```

To build just the benchmark with g++ directly:

```bash
g++ -std=c++17 -Wall -Wextra -O2 -Iinclude src/benchmark.cpp src/wal.cpp src/sstable.cpp src/bloomfilter.cpp src/compaction.cpp src/engine.cpp -o lsmstore_bench
./lsmstore_bench
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

## Benchmark results

These numbers are from an actual run of `lsmstore_bench` (5,000 writes,
then 5,000 reads of existing keys, then 5,000 reads of keys that never
existed), not estimated or made up. Your numbers will vary by machine,
and the caveat in `benchmark.cpp` applies: `FLUSH_THRESHOLD` (5) and
`COMPACTION_THRESHOLD` (4) are kept tiny for REPL demoability, so this
run triggers flush and compaction far more often than a real deployment
(which would size these in MB) would — these numbers include a lot more
flush/compaction overhead per operation than a tuned configuration
would see.

```
Write:                 5000 writes in 0.53s  =  9,465 writes/sec
Read (existing keys):  5000 reads  in 8.66s  =    577 reads/sec  (5000/5000 hits)
                        bloom filter skipped 0 file-scan(s) total
Read (missing keys):   5000 reads  in 0.13s  = 39,223 reads/sec
                        bloom filter skipped 4,962 file-scan(s) total (99.2% of reads)
```

**The interesting finding, and it wasn't the one I expected going in:**
reads for keys that *exist* are roughly 68x slower than reads for keys
that *don't*. At first glance that looks backwards — shouldn't finding
something be at least as fast as not finding it?

The reason is the bloom filter skipped zero files on the hit-read pass.
By the time reads ran, compaction had already merged everything down to
a single SSTable (5,000 writes crosses the compaction threshold many
times over), so every existing key is a "maybe present" for that one
file's filter — correctly, since it actually is present — which means
`lookupSSTable()` has to fall through to its linear, early-exit disk
scan every single time. For a *missing* key, the filter says "definitely
not present" immediately and the scan never happens at all, which is
exactly why the miss path is so much faster.

In other words: **the bloom filter is doing its job perfectly — it just
can't help on hits, only misses, and this benchmark's workload (reusing
a small pool of 5,000 keys) is almost entirely hits.** The real
remaining bottleneck is that a *hit* still means an O(n) linear scan of
a sorted file with no index to jump into it. That's exactly the gap a
sparse index (key → byte offset, sampled every N entries) would close —
not by avoiding the scan, but by shrinking it from "whole file" to "a
small window around where the key should be." That's the concrete,
benchmark-justified next step if this project continued.

This is also a real, measured example of the LSM-tree **read
amplification** tradeoff the design accepts in exchange for its write
path never having to sort data in place: every SSTable file is an extra
place a read might need to touch, and within a file, no index yet means
checking it costs more than it should.

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
- **Why point lookups do a linear scan with early exit, for SSTables
  without a usable filter**: SSTable entries are sorted, so a lookup can
  stop scanning the moment it passes the target key alphabetically — it
  doesn't need to read the rest of the file. This is still O(n) in the
  worst case though, which is exactly what the bloom filter below is for.
- **Bloom filter: the false-negative bug that almost shipped.** The
  filter's bit array gets padded up to a whole number of bytes when
  saved to disk (`(num_bits + 7) / 8` bytes). The first version of the
  reload path recomputed `num_bits` from that padded byte count instead
  of persisting the original value — which meant `add()` (at flush time)
  and `mightContain()` (after a restart) hashed the same key into two
  *different* bit positions, because they were reducing modulo two
  different numbers. That silently broke the one guarantee a bloom
  filter is never allowed to break: no false negatives. It only showed
  up after a restart, never in the same session, which is exactly the
  kind of bug that's easy to miss without deliberately testing
  persistence. Fixed by writing the exact `num_bits` used at build time
  into the filter file and reusing that same value on reload, instead of
  re-deriving it from the padded byte array.
- **Compaction's crash-safety ordering, and a related fix.** Compaction
  writes the merged SSTable and its bloom filter *completely*, syncs
  them to disk, and only then deletes the old files. If the process dies
  midway, you're left with either the full old set (delete never
  happened) or the full old set plus a harmless finished new file (delete
  was about to happen) — never a half-merged, half-deleted mess. That
  ordering is only a real guarantee if "written" actually means
  durable, though: `writeSSTable()` originally only called `flush()`
  and `close()`, which pushes data out of the C++ stream buffer but
  doesn't force the OS to commit it to disk. Under real power loss (not
  just a process crash), that gap meant the "new file is safe before we
  delete the old ones" promise wasn't actually true. Fixed by giving
  `writeSSTable()` the same `fsync`-equivalent call the WAL already
  used, so both the durability boundary and the crash-safety ordering
  compaction relies on are real, not just implied.
- **Why a skip list over `std::map`**: comparable O(log n) average-case
  performance, but a much simpler mental model, and it extends more
  naturally to lock-free / fine-grained-locking concurrent versions
  later, which real memtables need.
- **Why the engine got pulled out of `main()` into its own class.**
  Through Phase 4, the entire engine lived as local variables and lambdas
  inside `main()`, which was fine for a REPL but made it impossible to
  drive the engine any other way. Benchmarking needs thousands of timed
  operations in a loop, not one command typed at a time over stdin — so
  the engine needed a real API. `Engine` is that API: `main.cpp` is now a
  thin REPL that calls into it, and `benchmark.cpp` calls the exact same
  class with no duplicated logic. Confirmed the refactor changed no
  behavior by re-running the full compaction regression test (the same
  one that caught the bloom filter and fsync bugs) against the
  refactored engine and diffing the output against the pre-refactor run
  — identical.
