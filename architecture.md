# DedupAndCompEngine — Architecture

A deduplication and compression engine that saves disk space by ingesting files
(PDF, Office documents, images, video, or anything else) into a
content-addressed repository, storing each unique piece of data exactly once,
compressed. Files can be restored bit-for-bit at any time, and (in later
phases) accessed live through a virtual drive.

- **Engine**: C++ (performance-critical core: scanning, chunking, hashing, compression, storage)
- **Interface**: C# (command-line first, GUI later)
- **Platform**: Windows first; the engine and CLI target Windows, Linux and macOS (see §10)

**Status:** Phase 1 (walking skeleton) is complete. Phase 2 (real dedup) is next.
See §7 for the roadmap.

---

## 1. Goals and Non-Goals

### Goals
- Reclaim disk space from duplicate and near-duplicate files (copies, versions, backups).
- Compress unique data with a modern general-purpose codec (zstd).
- Guarantee lossless, verifiable restore (every restored file matches its original hash).
- Provide clear savings statistics: bytes in, unique bytes, compressed bytes, ratio.
- Keep the engine usable standalone (CLI) without the C# layer.
- Handle file names in any language (full Unicode) on every platform.
- Later: let any program read stored files live, without restoring them first (§9).

### Non-Goals (for v1)
- **In-place transparent access at the original path** (files remain openable from
  their original location while stored deduplicated). The v1 model is
  *archive-style*: ingest into a repository, then optionally delete originals.
  This is no longer considered out of reach: on Windows the Cloud Files API makes
  it feasible without a custom driver — planned as an optional late phase (§9.3).
- **Writable live access** in the first virtual-drive release; it ships read-only
  first (§9.1), with editing as a later phase (§9.2).
- **Format-aware recompression** (recompressing streams inside PDFs/JPEGs/ZIPs).
  Planned as an optional later phase (see §8).
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

### Positioning: how this differs from archivers (WinRAR, 7-Zip, ZIP)

Archivers pack a set of files into a standalone archive, mainly for sending or
bundling. This engine is a **long-lived, growing repository** in which every
ingest is deduplicated against everything ever stored. Its closest relatives
are deduplicating backup tools (restic, BorgBackup, Duplicacy), not archivers.

| | Archivers (WinRAR etc.) | This engine |
|---|---|---|
| Unit of storage | Many independent archives | One repository, grows over time |
| Dedup across ingests / over time | None — each archive stands alone | Yes — reuses any chunk ever stored |
| Adding an edited version later | Stored in full, or re-pack a solid archive | Only changed chunks stored (Phase 2) |
| Extracting one file | Can require decompressing preceding data (solid archives) | Reads only that file's chunks |
| Compression | Strong, slower (RAR/LZMA) | zstd: very fast, slightly lower ratio |
| Integrity | Checksums + optional recovery records (can *repair*) | BLAKE3 verification (*detects*, no repair) |
| Encryption, multi-volume, SFX | Yes | No |

Archivers can exploit redundancy *within* one solid archive (and can store
identical files as references), so for a one-off pack of a folder they may
produce a smaller result. The engine wins on repeated ingests over time and on
fast single-file access. Already-compressed archives (.rar/.zip) deduplicate
poorly as input, since compression hides shared content.

---

## 2. High-Level Architecture

```
┌──────────────────────────────────────────────┐
│              C# Interface Layer              │
│   CLI (Phase 3)  →  GUI (Phase 6)            │
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
│  └────┬─────┘  └────┬─────┘  └─────┬──────┘  │
│       │             │              │         │
│       │      ┌──────▼──────────────▼──────┐  │
│       │      │  Repository (chunk store,  │  │
│       │      │  packs + manifests on disk)│  │
│       │      └────────────────────────────┘  │
│  ┌────▼──────────────────┐                   │
│  │ Mount (Phase 5+):     │ ← WinFsp / FUSE   │
│  │ random-access reads   │   / macFUSE       │
│  └───────────────────────┘                   │
└──────────────────────────────────────────────┘
```

Data flows one direction on ingest (scan → chunk → hash → dedup-check →
compress → store) and the reverse on restore (manifest → chunk lookup →
decompress → concatenate → verify). The mount layer reuses the restore path
but reads arbitrary byte ranges on demand instead of whole files.

---

## 3. C++ Engine Components

### 3.1 Scanner
- Recursively walks the input paths (`std::filesystem`).
- Collects candidate files with size, mtime, and path.
- Skip rules: repository's own directory, symlinks/junctions (v1), zero-byte
  files (recorded in manifest without chunks), optional include/exclude globs.
- Fast-path change detection for re-runs (Phase 2): a file whose (path, size,
  mtime) matches a previous ingest can be skipped without re-reading
  (configurable, since mtime can lie; a `--paranoid` flag forces re-hashing).

### 3.2 Chunker — Content-Defined Chunking (FastCDC)
- **Phase 1:** whole-file mode — each file is a single chunk, so only exact
  duplicates deduplicate.
- **Phase 2:** splits each file stream into variable-size chunks at boundaries
  determined by content (rolling hash), not by fixed offsets. This is what makes
  dedup resilient to insertions/deletions between file versions:

  ```
  Fixed-size cuts:    [AAAA][BBBB][CCCC]  → insert "x" → [xAAA][ABBB][BCCC]   nothing matches
  Content-defined:    [AAAA][BBBB][CCCC]  → insert "x" → [xAAAA][BBBB][CCCC] only first chunk changes
  ```
- Parameters (defaults): min 16 KiB, average 64 KiB, max 256 KiB.
  Stored in repository config — **changing them breaks dedup against existing
  chunks**, so they are fixed per repository at init time.
- Files smaller than min chunk size become a single chunk.
- Small chunks are also what make live random access fast (§9): any byte of a
  file is at most one small chunk decompression away.

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
- Incompressibility guard (**implemented in Phase 1**): if compressed size ≥
  original size (typical for media), store the chunk raw with `codec = none`.
  This avoids wasting space on already-compressed data.
- Compression is per-chunk, so any chunk can be restored independently.

### 3.6 Repository (on-disk layout)

```
repo/
├── config.json          # format version, chunking mode + params, hash
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
- Chunks are written to a temporary file (`chunks/.tmp-*`) and renamed into
  place, so a crash never leaves a partially written chunk under its hash name.
- v2 optimization: **pack files** (append chunks into ~64 MiB packs, index
  stores pack ID + offset + length) to reduce filesystem overhead for millions
  of small chunks. The index schema is designed for this from day one.
- `config.json` records the chunking mode (`"whole-file"` in Phase 1). Phase 2
  engines **refuse to open whole-file repositories** with a clear message rather
  than mixing formats (no real data to migrate at this stage).

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
    path        TEXT NOT NULL,      -- original absolute path, UTF-8 (§3.11)
    size        INTEGER NOT NULL,
    mtime       INTEGER NOT NULL,
    file_hash   BLOB NOT NULL       -- whole-file BLAKE3 for restore verification
);

-- ordered chunk list per file (the "recipe" to rebuild it)
CREATE TABLE file_chunks (
    file_id     INTEGER NOT NULL REFERENCES files(id),
    seq         INTEGER NOT NULL,
    chunk_hash  BLOB NOT NULL REFERENCES chunks(hash),
    offset      INTEGER NOT NULL,   -- Phase 2: byte offset of this chunk within the file
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
- `refcount` supports deletion: `forget` (§3.12) decrements a file version's
  chunks; `gc` deletes chunks with refcount 0.
- `file_chunks.offset` (added in Phase 2) lets a reader map "byte N of file F"
  to one chunk with a single indexed lookup — required for the virtual drive's
  random-access reads (§9.1).
- Every ingest adds a new `files` row, so the index already keeps the history
  of each path; this is the basis for file version history (§9.2).

### 3.8 Restore pipeline
1. Look up the file's manifest (ordered chunk hashes).
2. For each chunk: locate, read, decompress, append to output file.
3. Verify the whole-file BLAKE3 against the stored `file_hash`.
   A mismatch is a hard error — never silently deliver corrupt data.

File lookup (`--file`) accepts an id, a full path, or a trailing name. Name
matching treats both `/` and `\` as separators (so it works for paths from any
OS), escapes SQL `LIKE` wildcards (`my_file.txt` never matches `myXfile.txt`),
and picks the newest version when several match.

### 3.9 Integrity: `verify` command (Phase 2)
- Re-reads every stored chunk, decompresses, re-hashes, compares to its ID.
- Run before trusting the repository enough to delete originals.

### 3.10 Concurrency model (Phase 4)
- Pipeline parallelism: reader thread → N worker threads (chunk+hash+compress)
  → single writer thread (chunk store + SQLite). SQLite stays single-writer,
  which keeps transactions simple.
- Bounded queues between stages so memory stays flat on huge files.
- The mount layer (§9) is read-mostly: concurrent readers use SQLite's
  read concurrency plus a shared decompressed-chunk cache.

### 3.11 Text encoding and paths
- **UTF-8 everywhere inside the engine.** Paths are stored in the index as UTF-8
  and converted explicitly at the boundary (`engine/src/utf8.h`:
  `path_to_utf8` / `path_from_utf8`), so storage does not depend on a compiler's
  native narrow encoding (MSVC would otherwise use the legacy ANSI code page).
- **Windows command line:** `argv` is delivered in the ANSI code page, which
  turns non-ASCII names into `?`. The engine re-reads the wide command line
  (`CommandLineToArgvW`) and converts it to UTF-8. Other OSes already pass UTF-8.
- **Long paths (> 260 chars) on Windows:** enabled via the `longPathAware`
  manifest that the MinGW toolchain embeds by default. Do not add a second
  application manifest — the linker rejects duplicates (an embedded UTF-8
  `activeCodePage` manifest was tried and dropped for this reason).
- **Open issue — cross-OS repositories:** paths are stored with their native
  separators (`C:\Docs\a.pdf`). A repository ingested on Windows and restored on
  Linux/macOS would produce files named literally `C:\Docs\a.pdf`. Fix: store a
  normalized generic form (`/` separators plus a recorded source-OS/volume
  field) and convert on restore. Needed before repositories are moved between OSes.

### 3.12 Removing data: `forget` + `gc` (Phase 2)
`gc` alone can never free anything, because nothing lowers refcounts. Phase 2
therefore adds:
- `forget <repo> --file <id|path|name> | --job <id>` — deletes the selected file
  versions from the index and decrements their chunks' refcounts, in one
  transaction.
- `gc <repo>` — deletes chunk files whose refcount is 0, and leftover
  `chunks/.tmp-*` files from interrupted ingests.

---

## 4. Engine CLI (contract for the C# layer)

```
dedupc init    <repo>                                   # create repository
dedupc ingest  <repo> <path>... [--level N] [--paranoid] [--json]
dedupc restore <repo> --file <id|path|name> --out <dir> [--json]
dedupc list    <repo> [--json]                          # ingested files + jobs
dedupc stats   <repo> [--json]                          # savings summary
dedupc verify  <repo> [--json]                          # Phase 2
dedupc forget  <repo> --file <id|path|name> | --job <id> [--json]   # Phase 2
dedupc gc      <repo> [--json]                          # Phase 2: drop unreferenced chunks
dedupc mount   <repo> <mountpoint> [--writable]         # Phase 5 (read-only); --writable in Phase 7
```

Implemented today: `init`, `ingest` (without `--paranoid`/`--json`),
`restore`, `list`, `stats`.

With `--json`, the engine emits **JSON Lines on stdout** — one object per
event — which is the machine interface the C# layer consumes:

```json
{"event":"progress","phase":"ingest","file":"C:\\docs\\a.pdf","done_bytes":1048576,"total_bytes":52428800}
{"event":"file_done","file":"C:\\docs\\a.pdf","raw":52428800,"deduped":41943040,"stored":9175040}
{"event":"job_done","raw_bytes":1073741824,"unique_bytes":412316860,"stored_bytes":268435456,"ratio":4.0}
{"event":"error","code":"EACCES","file":"C:\\docs\\locked.xlsx","message":"access denied"}
```

JSON output is UTF-8 regardless of console code page.

Exit codes: `0` success, `1` completed with per-file errors, `2` fatal.

---

## 5. C# Interface Layer

### Phase 3: Command-line front-end (`dedup-cli`)
- .NET 8 console app (runs on Windows, Linux and macOS).
- Spawns `dedupc --json`, parses JSON lines, renders progress bars and a
  final savings report.
- Adds user-facing conveniences: config profiles, recently used repos,
  human-readable sizes.

### Phase 6: GUI
- Same subprocess protocol — no engine changes needed.
- Folder picker, live progress, savings dashboard (before/after chart),
  browse/restore ingested files, one-click verify, mount/unmount the virtual drive.
- **Framework choice decides portability:** WPF and WinUI 3 are Windows-only.
  **Avalonia** (XAML, WPF-like) runs on Windows, Linux and macOS and is the
  recommended choice if the GUI should be cross-platform. .NET MAUI lacks
  official desktop-Linux support. Decide before Phase 6 starts.

### Interop choice: subprocess vs P/Invoke
**v1 uses subprocess + JSON lines**, deliberately:
- Engine and UI can be developed and debugged completely independently.
- An engine crash cannot take down the UI.
- The engine remains a useful standalone tool.

P/Invoke against a flat-C API DLL is a possible later optimization if
per-call latency ever matters (it likely won't — jobs are long-running).

---

## 6. Third-Party Libraries

| Library    | Purpose            | License / notes                         |
|------------|--------------------|-----------------------------------------|
| zstd       | Compression        | BSD; simple C API (FetchContent)        |
| BLAKE3     | Hashing            | CC0 / Apache-2.0 (FetchContent)         |
| SQLite     | Index + manifests  | Public domain; vendored amalgamation    |
| FastCDC    | Chunking           | Implemented in-project (~200 lines) per the FastCDC paper |
| nlohmann/json or a minimal writer | JSON output | Header-only (MIT)   |
| WinFsp     | Virtual drive on Windows (Phase 5) | GPLv3 with a FLOSS exception; commercial license otherwise — **check before distributing**. Dokany (LGPL/MIT) is an alternative |
| libfuse    | Virtual drive on Linux (Phase 5)   | LGPL                       |
| macFUSE / FUSE-T | Virtual drive on macOS (Phase 5) | macFUSE's license restricts redistribution — **check**; FUSE-T is an alternative |

Core libraries (zstd, BLAKE3, SQLite) are permissively licensed. The
virtual-drive libraries are not all permissive: their licenses must be reviewed
before the mount feature is distributed.

Build: **CMake** + FetchContent for dependencies; MinGW-w64 GCC or MSVC on
Windows, GCC/Clang elsewhere. Single static `dedupc.exe` with MinGW.
Tests: PowerShell round-trip test today; Catch2 or GoogleTest for engine unit
tests; xUnit for the C# layer.

---

## 7. Implementation Phases

Each phase ends with a working, shippable tool.

1. **Phase 1 — Walking skeleton** ✅ *complete*
   C++ CLI: `init`, `ingest` with *whole-file* dedup (hashed + zstd-compressed,
   raw fallback for incompressible data), `restore` with hash verification,
   `list`, `stats`. Round-trip test (ingest → restore → byte-identical).
   Unicode file names and OS-independent restore name lookup.

2. **Phase 2 — Real dedup** *(next)*, in this order:
   1. FastCDC chunker as its own source file, with a unit test.
   2. Multi-chunk ingest/restore; `file_chunks.offset`; refuse whole-file repos.
      Round-trip test gains a **near-duplicate check** (an edited copy must store
      far less than a full copy).
   3. `verify`.
   4. `forget` + `gc` (§3.12).
   5. Re-run fast path + `--paranoid`.

3. **Phase 3 — C# CLI**
   `--json` JSON-lines protocol in the engine; .NET 8 front-end with progress
   and reports.

4. **Phase 4 — Hardening & performance**
   Multithreaded pipeline, pack files, large-scale tests (100 GB+ datasets),
   crash-recovery tests, cross-OS repository paths (§3.11 open issue),
   Linux/macOS builds in CI.

5. **Phase 5 — Read-only virtual drive** (`dedupc mount`, §9.1)
   WinFsp on Windows, FUSE/macFUSE elsewhere. Requires Phase 2 chunking.

6. **Phase 6 — GUI** (Avalonia recommended, §5): dashboard, browse/restore,
   verify, mount/unmount.

7. **Phase 7 (optional) — Writable virtual drive + version history** (§9.2).

8. **Phase 8 (optional, Windows-only) — In-place placeholders** via the Cloud
   Files API (§9.3).

9. **Phase 9 (optional) — Format-aware recompression**, see §8.

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

## 9. Live Access: Using Stored Files Without Restoring

Goal: any program (editor, PDF reader, video player, script) can use a stored
file directly, without an explicit `restore` that copies it out first.

### 9.1 Read-only virtual drive (Phase 5)

`dedupc mount <repo> R:` makes the repository appear as a drive letter (a
directory mount point on Linux/macOS) showing every ingested file under its
original folder structure. Stopping the command removes the drive; the
repository is unchanged. Nothing is written to disk — data is decompressed into
memory only as it is read. (Comparable to how Windows mounts an `.iso` as a
virtual DVD drive, or `restic mount` / `borg mount`.)

**How a read is served:**

```
Program: "read 4096 bytes of report.pdf at offset 1,000,000"
   → OS → WinFsp / FUSE driver (ready-made, signed; we write no kernel code)
   → dedupc mount process:
       1. resolve path → file id                       (files table)
       2. offset → chunk                               (file_chunks.offset, indexed)
       3. decompress that chunk (LRU cache of decompressed chunks)
       4. return the requested byte range
```

- The driver library forwards filesystem calls (list directory, get
  attributes, open, read) to our normal user-mode process. A bug crashes only
  `dedupc`, never the OS.
- **Directory tree:** built from `files.path`; when a path has several versions,
  the newest is shown (older versions could later appear under a
  `.versions/` folder).
- **Writes are rejected** (`access denied` / read-only media). Editors can still
  "Save As" to a normal location.
- **Depends on Phase 2:** with whole-file chunks, reading byte N of a 2 GB file
  would require decompressing everything before it. With ~64 KiB chunks, any
  offset is one small decompression away, so seeking (e.g. in video) stays fast.
- One implementation covers Windows (WinFsp), Linux (libfuse) and macOS
  (macFUSE/FUSE-T); their APIs are nearly identical. See §6 for license checks.

### 9.2 Writable virtual drive + version history (Phase 7, optional)

Lets a user open `R:\Docs\notes.txt` in an editor, change it and save in place;
the engine stores only the changed chunks.

**Why it is hard:** programs save in very different ways — some overwrite in
place; many "safe save" (write a temp file, delete the original, rename); Word
creates lock files (`~$x.docx`) and several temp files; others patch a few bytes
mid-file. The engine sees only a stream of create/write/rename/delete calls and
must never end up with a half-saved file after a crash.

**Design: copy-on-write staging**
1. When a file is opened for writing, restore a full copy into a hidden staging
   area on a real disk.
2. All writes, renames and temp files go to the staging area (ordinary files,
   so every program behaves normally).
3. When the file is closed, ingest the staged file as a **new version**:
   chunk it, reuse unchanged chunks, store only the changed ones.
4. Delete the staged copy; the drive now shows the new version.

**Version history** falls out naturally: unchanged chunks are shared, so keeping
old versions costs almost nothing ("restore notes.txt as of last Tuesday").

**Limits:** opening a very large file for writing stages the whole file first
(slow for multi-GB video; fine for documents); concurrent writers to one file
need locking; each major application's save pattern needs testing.

Until then, the manual equivalent is `restore` → edit → `ingest`; after Phase 2
that re-ingest stores only the changed chunks.

### 9.3 In-place placeholders (Phase 8, optional, Windows-only)

The original file keeps its original path (`C:\Docs\report.pdf`) but becomes a
near-zero-size **placeholder**; when a program opens it, Windows asks the engine
for the data and fills the file in. This is the mechanism behind OneDrive
"Files On-Demand" — the **Windows Cloud Files API** (user-mode, no custom
driver, Windows 10/11).

- Advantage: shortcuts, recent-file lists and app settings pointing at the
  original paths keep working.
- Drawbacks: an opened file is hydrated back to full size until dehydrated
  again; Windows-only (Linux/macOS need a different mechanism).
- Note: Windows **Server** has built-in Data Deduplication at the filesystem
  level; it is not available on client editions such as Windows 11 Home.

### 9.4 Comparison

| | `restore` (today) | Read-only drive (P5) | Writable drive (P7) | Placeholders (P8) |
|---|---|---|---|---|
| Any program can open files | After copying out | ✓ new drive letter | ✓ new drive letter | ✓ **original path** |
| Extra disk space | Full copy | None | Staging while editing | Full size while hydrated |
| Save edits in place | On the copy | ✗ | ✓ (only changed chunks stored) | Possible, complex |
| Platforms | All | All | All | Windows only |
| Difficulty | Done | Medium | High | High |

---

## 10. Cross-Platform Support

| Part | Windows | Linux | macOS | Notes |
|---|---|---|---|---|
| C++ engine | ✓ | ✓ | ✓ | Standard C++20 only; no Windows APIs except UTF-8 argv handling (`#ifdef _WIN32`) |
| zstd / BLAKE3 / SQLite | ✓ | ✓ | ✓ | |
| Repository format | ✓ | ✓ | ✓ | Same bytes everywhere; cross-OS *path* handling is an open issue (§3.11) |
| Build | ✓ | ✓ | ✓ | CMake; `build.ps1` and test scripts assume Windows paths — run CMake directly elsewhere |
| C# CLI (.NET 8) | ✓ | ✓ | ✓ | |
| GUI | ✓ | Avalonia only | Avalonia only | WPF/WinUI would be Windows-only (§5) |
| Virtual drive | WinFsp | libfuse | macFUSE / FUSE-T | |
| In-place placeholders | ✓ | — | — | Cloud Files API is Windows-only |
| Locked files (VSS) | Optional | — | — | Volume Shadow Copy is Windows-only |

Known portability items:
- `clock_cast` (file mtime conversion, `commands.cpp`) is supported by GCC's
  library; Apple's libc++ may need a small fallback.
- Cross-OS repository paths (§3.11).

---

## 11. Key Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Data loss / corrupt restore | Whole-file hash verification on every restore; `verify` command; never delete originals automatically |
| Repository corruption on crash | SQLite transactions; chunks written to temp files and renamed before the index references them; `gc` cleans orphans |
| Poor savings on media-heavy folders | Set expectations in stats output (dedup vs compression breakdown); incompressibility guard keeps ingest fast |
| Chunker parameter drift breaking dedup | Parameters frozen in `config.json` at repo init |
| Mixing repository formats | Chunking mode recorded in `config.json`; engine refuses repositories it was not built for |
| Locked/in-use files on Windows | Per-file error events, job continues; optional VSS snapshot support later |
| Millions of small chunk files slow on NTFS | Fan-out directories in v1; pack files in Phase 4 |
| Non-ASCII file names mangled | UTF-8 throughout; wide command line on Windows (§3.11) |
| Repository moved between OSes restores wrong names | Normalized path storage (Phase 4, §3.11) |
| Half-saved files through the writable drive | Copy-on-write staging; new version committed only on close, in one transaction (§9.2) |
| Virtual-drive library licensing | Review WinFsp / macFUSE licenses before distribution; Dokany / FUSE-T as alternatives (§6) |
| Space can never be reclaimed | `forget` + `gc` in Phase 2 (§3.12) |
