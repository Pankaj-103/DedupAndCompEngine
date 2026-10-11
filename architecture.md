# DedupAndCompEngine — Architecture

A deduplication and compression engine that saves disk space by ingesting files
(PDF, Office documents, images, video, or anything else) into a
content-addressed repository, storing each unique piece of data exactly once,
compressed. Files can be restored bit-for-bit at any time.

- **Engine**: C++ (performance-critical core: scanning, chunking, hashing, compression, storage)
- **Interface**: C# (command-line first, optional GUI later)
- **Platform**: Windows first, but the engine avoids Windows-only APIs where practical

---

## 1. Goals and Non-Goals

### Goals
- Reclaim disk space from duplicate and near-duplicate files (copies, versions, backups).
- Compress unique data with a modern general-purpose codec (zstd).
- Guarantee lossless, verifiable restore (every restored file matches its original hash).
- Provide clear savings statistics: bytes in, unique bytes, compressed bytes, ratio.
- Keep the engine usable standalone (CLI) without the C# layer.

### Non-Goals (for v1)
- **In-place transparent compression** (files remain openable from their original
  location while stored deduplicated). This requires filesystem filter drivers /
  reparse points on Windows and is out of scope. The v1 model is
  *archive-style*: ingest into a repository, then optionally delete originals.
- **Format-aware recompression** (recompressing streams inside PDFs/JPEGs/ZIPs).
  Planned as a later phase (see §8).
- Encryption, networking/cloud sync, multi-user access.

### Reality check that shapes the design
Most target formats (PDF, xlsx/pptx/docx, JPEG/PNG, MP4) are **already
compressed internally**, so general compression gains only ~1–3% on them.
The dominant savings come from **deduplication**:

| Data situation                          | Expected savings source        | Typical result |
|-----------------------------------------|--------------------------------|----------------|
| Exact duplicate files                   | File/chunk dedup               | ~100%          |
| Versioned documents (v1 vs v2 of a PPT) | Chunk-level dedup (FastCDC)    | 20–60%         |
| Text, CSV, logs, source code, DB dumps  | zstd compression               | 50–80%         |
| Unique media (photos, video)            | Compression (marginal)         | 1–3%           |

Therefore dedup is the core of the engine; compression is a second-stage
benefit applied to unique chunks.

---

## 2. High-Level Architecture

```
┌──────────────────────────────────────────────┐
│              C# Interface Layer              │
│   CLI (v1)  →  GUI (WPF/WinUI, later)        │
│   - folder selection, job control            │
│   - progress display, savings statistics     │
└───────────────────┬──────────────────────────┘
                    │  subprocess + JSON-lines protocol (v1)
                    │  (option: P/Invoke into engine DLL later)
┌───────────────────▼──────────────────────────┐
│              C++ Engine (dedupc.exe)         │
│                                              │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐  │
│  │ Scanner  │→ │ Chunker  │→ │  Hasher    │  │
│  │ (fs walk)│  │ (FastCDC)│  │ (BLAKE3)   │  │
│  └──────────┘  └──────────┘  └─────┬──────┘  │
│                                    │ new chunk?
│  ┌──────────┐  ┌──────────┐  ┌─────▼──────┐  │
│  │ Restore  │← │  Index   │← │ Compressor │  │
│  │ pipeline │  │ (SQLite) │  │  (zstd)    │  │
│  └──────────┘  └────┬─────┘  └─────┬──────┘  │
│                     │              │         │
│              ┌──────▼──────────────▼──────┐  │
│              │  Repository (chunk store,  │  │
│              │  packs + manifests on disk)│  │
│              └────────────────────────────┘  │
└──────────────────────────────────────────────┘
```

Data flows one direction on ingest (scan → chunk → hash → dedup-check →
compress → store) and the reverse on restore (manifest → chunk lookup →
decompress → concatenate → verify).

---

## 3. C++ Engine Components

### 3.1 Scanner
- Recursively walks the input paths (`std::filesystem`).
- Collects candidate files with size, mtime, and path.
- Skip rules: repository's own directory, symlinks/junctions (v1), zero-byte
  files (recorded in manifest without chunks), optional include/exclude globs.
- Fast-path change detection for re-runs: a file whose (path, size, mtime)
  matches a previous ingest can be skipped without re-reading (configurable,
  since mtime can lie; a `--paranoid` flag forces re-hashing).

### 3.2 Chunker — Content-Defined Chunking (FastCDC)
- Splits each file stream into variable-size chunks at boundaries determined by
  content (rolling hash), not by fixed offsets. This is what makes dedup
  resilient to insertions/deletions between file versions.
- Parameters (v1 defaults): min 16 KiB, average 64 KiB, max 256 KiB.
  Stored in repository config — **changing them breaks dedup against existing
  chunks**, so they are fixed per repository at init time.
- Files smaller than min chunk size become a single chunk.

### 3.3 Hasher
- **BLAKE3** for chunk IDs: fast, 256-bit, cryptographically strong — chunk ID
  collisions can be treated as impossible, so equal hash ⇒ equal content, no
  byte-compare needed.
- A whole-file BLAKE3 hash is also computed during ingest and stored in the
  manifest; restore verifies against it.

### 3.4 Deduplicator
- For each chunk hash, consult the index:
  - **Hit** → record a reference to the existing chunk; the data is dropped.
  - **Miss** → hand the chunk to the compressor, then store it and index it.
- Statistics per job: raw bytes seen, bytes deduplicated, unique bytes stored.

### 3.5 Compressor
- **zstd**, default level 3 (fast) with `--level` override up to 19.
- Incompressibility guard: if compressed size ≥ original size (typical for
  media chunks), store the chunk raw with a `codec = none` flag. This avoids
  wasting CPU and space on already-compressed data.
- Compression is per-chunk, so any chunk can be restored independently.

### 3.6 Repository (on-disk layout)

```
repo/
├── config.json          # format version, chunker params, created date
├── index.db             # SQLite: chunk index + file manifests
├── chunks/
│   ├── ab/
│   │   └── abcdef12...  # one file per chunk, named by hex hash (v1)
│   └── cd/
│       └── cdef3456...
└── locks/
```

- v1 stores each chunk as its own file under a 2-hex-char fan-out directory
  (256 subdirs) to keep directory sizes manageable.
- v2 optimization: **pack files** (append chunks into ~64 MiB packs, index
  stores pack ID + offset + length) to reduce filesystem overhead for millions
  of small chunks. The index schema is designed for this from day one.

### 3.7 Index (SQLite)

```sql
-- every unique chunk in the repository
CREATE TABLE chunks (
    hash        BLOB PRIMARY KEY,   -- 32-byte BLAKE3
    raw_size    INTEGER NOT NULL,
    stored_size INTEGER NOT NULL,
    codec       INTEGER NOT NULL,   -- 0 = none, 1 = zstd
    pack_id     INTEGER,            -- NULL in v1 (loose chunk files)
    pack_offset INTEGER,
    refcount    INTEGER NOT NULL DEFAULT 0
);

-- every ingested file version
CREATE TABLE files (
    id          INTEGER PRIMARY KEY,
    job_id      INTEGER NOT NULL,
    path        TEXT NOT NULL,      -- original absolute path
    size        INTEGER NOT NULL,
    mtime       INTEGER NOT NULL,
    file_hash   BLOB NOT NULL       -- whole-file BLAKE3 for restore verification
);

-- ordered chunk list per file (the "recipe" to rebuild it)
CREATE TABLE file_chunks (
    file_id     INTEGER NOT NULL REFERENCES files(id),
    seq         INTEGER NOT NULL,
    chunk_hash  BLOB NOT NULL REFERENCES chunks(hash),
    PRIMARY KEY (file_id, seq)
);

-- ingest runs, for stats and history
CREATE TABLE jobs (
    id          INTEGER PRIMARY KEY,
    started_at  INTEGER NOT NULL,
    finished_at INTEGER,
    raw_bytes   INTEGER NOT NULL DEFAULT 0,
    stored_bytes INTEGER NOT NULL DEFAULT 0
);
```

- All ingest writes happen in transactions; a crash mid-ingest leaves the
  repository consistent (orphan chunk files are harmless and cleaned by `gc`).
- `refcount` supports deletion: removing a file version decrements its chunks;
  `gc` deletes chunks with refcount 0.

### 3.8 Restore pipeline
1. Look up the file's manifest (ordered chunk hashes).
2. For each chunk: locate, read, decompress, append to output file.
3. Verify the whole-file BLAKE3 against the stored `file_hash`.
   A mismatch is a hard error — never silently deliver corrupt data.

### 3.9 Integrity: `verify` command
- Re-reads every stored chunk, decompresses, re-hashes, compares to its ID.
- Run before trusting the repository enough to delete originals.

### 3.10 Concurrency model
- Pipeline parallelism: reader thread → N worker threads (chunk+hash+compress)
  → single writer thread (chunk store + SQLite). SQLite stays single-writer,
  which keeps transactions simple.
- Bounded queues between stages so memory stays flat on huge files.

---

## 4. Engine CLI (contract for the C# layer)

```
dedupc init   <repo>                          # create repository
dedupc ingest <repo> <path>... [--level N] [--paranoid] [--json]
dedupc restore <repo> --file <id|path> --out <dir> [--json]
dedupc list   <repo> [--json]                 # ingested files + jobs
dedupc stats  <repo> [--json]                 # savings summary
dedupc verify <repo> [--json]
dedupc gc     <repo> [--json]                 # drop unreferenced chunks
```

With `--json`, the engine emits **JSON Lines on stdout** — one object per
event — which is the machine interface the C# layer consumes:

```json
{"event":"progress","phase":"ingest","file":"C:\\docs\\a.pdf","done_bytes":1048576,"total_bytes":52428800}
{"event":"file_done","file":"C:\\docs\\a.pdf","raw":52428800,"deduped":41943040,"stored":9175040}
{"event":"job_done","raw_bytes":1073741824,"unique_bytes":412316860,"stored_bytes":268435456,"ratio":4.0}
{"event":"error","code":"EACCES","file":"C:\\docs\\locked.xlsx","message":"access denied"}
```

Exit codes: `0` success, `1` completed with per-file errors, `2` fatal.

---

## 5. C# Interface Layer

### v1: Command-line front-end (`dedup-cli`)
- .NET 8 console app.
- Spawns `dedupc.exe --json`, parses JSON lines, renders progress bars and a
  final savings report.
- Adds user-facing conveniences: config profiles, recently used repos,
  human-readable sizes.

### v2: GUI (WPF or WinUI 3)
- Same subprocess protocol — no engine changes needed.
- Folder picker, live progress, savings dashboard (before/after chart),
  browse/restore ingested files, one-click verify.

### Interop choice: subprocess vs P/Invoke
**v1 uses subprocess + JSON lines**, deliberately:
- Engine and UI can be developed and debugged completely independently.
- An engine crash cannot take down the UI.
- The engine remains a useful standalone tool.

P/Invoke against a flat-C API DLL is a possible later optimization if
per-call latency ever matters (it likely won't — jobs are long-running).

---

## 6. Third-Party Libraries (all permissive licenses)

| Library    | Purpose            | Notes                                   |
|------------|--------------------|-----------------------------------------|
| zstd       | Compression        | Industry standard; simple C API (BSD)   |
| BLAKE3     | Hashing            | Official C implementation (CC0/Apache)  |
| SQLite     | Index + manifests  | Single-file amalgamation (public domain)|
| FastCDC    | Chunking           | Implemented in-project (~200 lines) per the FastCDC paper |
| nlohmann/json or a minimal writer | JSON output | Header-only (MIT)   |

Build: **CMake** + vcpkg (or FetchContent) for dependencies; MSVC on Windows.
Tests: Catch2 or GoogleTest for the engine; xUnit for the C# layer.

---

## 7. Implementation Phases

1. **Phase 1 — Walking skeleton (useful on day one)**
   C++ CLI: `init`, `ingest` with *whole-file* dedup (file = one logical
   chunk sequence, still hashed + zstd-compressed), `restore`, `stats`.
   End-to-end round-trip test: ingest → restore → byte-identical.
2. **Phase 2 — Real dedup**
   FastCDC chunker, chunk-level index, refcounts, `gc`, `verify`,
   incompressibility guard, re-run fast path.
3. **Phase 3 — C# CLI**
   JSON-lines protocol, .NET front-end with progress and reports.
4. **Phase 4 — Hardening & performance**
   Multithreaded pipeline, pack files, large-scale tests (100 GB+ datasets),
   crash-recovery tests.
5. **Phase 5 — GUI** (WPF/WinUI dashboard).
6. **Phase 6 (optional) — Format-aware recompression**, see §8.

Each phase ends with a working, shippable tool.

---

## 8. Future: Format-Aware Recompression

The only way to beat the 1–3% ceiling on already-compressed formats:

- **PDF / ZIP / Office files**: decompress internal Flate streams, recompress
  with zstd/LZMA (must be perfectly reversible to reproduce the original file
  byte-for-byte — this is the hard part; see `precomp`/`antiz` prior art).
- **JPEG**: lossless recompression (Lepton-style) can save ~20%.
- Architecturally this becomes a **transform step before the chunker**:
  `file → (reversible transform) → chunk → hash → compress`. The manifest
  records which transform was applied so restore can invert it.
- Deliberately excluded from v1: correctness risk is concentrated here, and
  the transform must be bit-exact reversible across all real-world files.

---

## 9. Key Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Data loss / corrupt restore | Whole-file hash verification on every restore; `verify` command; never delete originals automatically |
| Repository corruption on crash | SQLite transactions; chunks written before index references them; `gc` cleans orphans |
| Poor savings on media-heavy folders | Set expectations in stats output (dedup vs compression breakdown); incompressibility guard keeps ingest fast |
| Chunker parameter drift breaking dedup | Parameters frozen in `config.json` at repo init |
| Locked/in-use files on Windows | Per-file error events, job continues; optional VSS snapshot support later |
| Millions of small chunk files slow on NTFS | Fan-out directories in v1; pack files in Phase 4 |
