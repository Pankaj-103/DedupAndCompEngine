# Recreates testdata/ for manual debugging: a sample input folder and a fresh,
# empty repository. Safe to re-run any time; testdata/ is git-ignored.
#
#   testdata/input/     files to ingest (duplicates, near-duplicate, binary, empty)
#   testdata/repo/      initialized dedupc repository
#   testdata/restored/  restore target

param(
    [string]$Dedupc = (Join-Path $PSScriptRoot "..\build\engine\dedupc.exe")
)

$ErrorActionPreference = "Stop"
$root = Join-Path $PSScriptRoot "..\testdata"

if (Test-Path $root) { Remove-Item -Recurse -Force $root }
$in = Join-Path $root "input"
New-Item -ItemType Directory -Force (Join-Path $in "nested") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $root "restored") | Out-Null

# Compressible text, plus an exact copy -> dedup hit.
$lines = 1..2000 | ForEach-Object { "line $_ : the quick brown fox jumps over the lazy dog" }
Set-Content -Encoding ascii (Join-Path $in "notes.txt") $lines
Copy-Item (Join-Path $in "notes.txt") (Join-Path $in "notes-copy.txt")

# Near-duplicate: one line changed. Phase 1 stores it in full (whole-file
# chunking); Phase 2's FastCDC should store only the changed region.
$edited = $lines.Clone()
$edited[1000] = "line 1001 : EDITED"
Set-Content -Encoding ascii (Join-Path $in "notes-v2.txt") $edited

# Incompressible random binary, plus a copy in a subfolder.
$bytes = New-Object byte[] (512KB)
(New-Object System.Random 42).NextBytes($bytes)
[IO.File]::WriteAllBytes((Join-Path $in "photo.bin"), $bytes)
Copy-Item (Join-Path $in "photo.bin") (Join-Path $in "nested\photo-backup.bin")

# Empty file: recorded with no chunks.
New-Item -ItemType File (Join-Path $in "empty.dat") | Out-Null

if (-not (Test-Path $Dedupc)) {
    Write-Warning "dedupc not built yet ($Dedupc); input created, repo not initialized."
    exit 0
}
& $Dedupc init (Join-Path $root "repo")
if ($LASTEXITCODE -ne 0) { Write-Error "dedupc init failed" }
Write-Host "testdata ready: $((Resolve-Path $root).Path)" -ForegroundColor Green
