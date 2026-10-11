# DedupAndCompEngine

A deduplication and compression engine that reclaims disk space by ingesting
files into a content-addressed repository: every unique piece of data is stored
exactly once, compressed with zstd, and any file can be restored bit-for-bit
with hash verification.

- **Engine**: C++20 (`dedupc` CLI) — scanning, hashing (BLAKE3), compression
  (zstd), content-addressed storage (SQLite index)
- **Interface**: C# front-end (planned, Phase 3)

See [architecture.md](architecture.md) for the full design.

## Status: Phase 1 — walking skeleton

Whole-file deduplication + per-file zstd compression, with verified restore.
Content-defined chunking (FastCDC) for near-duplicate detection lands in
Phase 2.

## Build

Requires CMake ≥ 3.24, Ninja, a C++20 compiler, and network access on first
configure (zstd and BLAKE3 are pulled via FetchContent; SQLite is vendored).

The easy way:

```powershell
.\build.ps1            # configure (first run only) + build
.\build.ps1 -Test      # build, then run the round-trip test
.\build.ps1 -Clean     # wipe the build folder and rebuild from scratch
```

The script finds cmake/ninja/g++ on PATH, or falls back to a portable
toolchain under `%USERPROFILE%\tools` (override with `-ToolsRoot <dir>`).

The manual way:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build
```

The engine binary lands at `build/engine/dedupc.exe` and is fully
self-contained (statically linked — no runtime DLLs needed).

### Toolchain

Any C++20 toolchain works (MSVC included). For a no-admin portable setup,
download and extract into `%USERPROFILE%\tools`:

| Tool | Source | Extracts to |
|---|---|---|
| CMake (windows-x86_64 zip) | github.com/Kitware/CMake/releases | `tools\cmake-<ver>-windows-x86_64\` |
| Ninja (ninja-win.zip) | github.com/ninja-build/ninja/releases | `tools\ninja\` |
| GCC (winlibs x86_64 ucrt zip) | github.com/brechtsanders/winlibs_mingw/releases | `tools\mingw64\` |

`build.ps1` picks these up automatically. To make the tools available in
*every* terminal (so plain `cmake`/`g++` work without the script), add them to
your **user** PATH environment variable once — no admin rights needed:

```powershell
$tools = "$env:USERPROFILE\tools"
$add = "$tools\cmake-4.3.3-windows-x86_64\bin;$tools\ninja;$tools\mingw64\bin"
[Environment]::SetEnvironmentVariable(
    "Path",
    [Environment]::GetEnvironmentVariable("Path", "User") + ";" + $add,
    "User")
```

This edits the same thing as Settings → System → About → *Advanced system
settings* → *Environment Variables…* → **User variables** → `Path` → Edit →
New (paste the three folder paths there if you prefer clicking). Either way it
takes effect in **new** terminal windows only — restart your terminal (and
VS Code, if you use its integrated terminal) after setting it. Verify with:

```powershell
cmake --version; ninja --version; g++ --version
```

## Usage

```
dedupc init    <repo>                          # create a repository
dedupc ingest  <repo> <path>... [--level N]    # ingest files/folders (zstd level 1-19, default 3)
dedupc restore <repo> --file <id|name> --out <dir>
dedupc list    <repo>                          # show ingested files
dedupc stats   <repo>                          # savings summary
```

Example:

```
dedupc init D:\repo
dedupc ingest D:\repo C:\Users\me\Documents
dedupc stats D:\repo
dedupc restore D:\repo --file report.pdf --out C:\restored
```

Every restore re-hashes the output and compares it against the BLAKE3 recorded
at ingest time — a corrupt restore fails loudly rather than silently.

## Test

```
powershell -File scripts\roundtrip-test.ps1 -Dedupc build\engine\dedupc.exe
```

Ingests a generated dataset (text, duplicates, incompressible binary, empty
file), restores everything, and verifies byte-for-byte identity.
