param(
    [Parameter(Mandatory = $true)][string]$Dedupc
)

$ErrorActionPreference = "Stop"
$fail = 0

$work = Join-Path $env:TEMP ("dedupc-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$data = Join-Path $work "data"
$repo = Join-Path $work "repo"
$restored = Join-Path $work "restored"
New-Item -ItemType Directory -Force $data | Out-Null

function Check($name, $cond) {
    if ($cond) { Write-Host "PASS  $name" -ForegroundColor Green }
    else { Write-Host "FAIL  $name" -ForegroundColor Red; $script:fail++ }
}

# --- build test dataset ---------------------------------------------------
# compressible text
$para = "The quick brown fox jumps over the lazy dog. " * 2000
Set-Content -Path (Join-Path $data "text.txt") -Value $para -Encoding ascii
# exact duplicate under another name
Copy-Item (Join-Path $data "text.txt") (Join-Path $data "text-copy.txt")
# incompressible pseudo-random binary (~2 MiB)
$rand = New-Object byte[] (2MB)
(New-Object Random 42).NextBytes($rand)
[IO.File]::WriteAllBytes((Join-Path $data "random.bin"), $rand)
# empty file
New-Item -ItemType File (Join-Path $data "empty.dat") | Out-Null
# nested folder
New-Item -ItemType Directory (Join-Path $data "nested") | Out-Null
Copy-Item (Join-Path $data "random.bin") (Join-Path $data "nested\random-dup.bin")

# --- init + ingest --------------------------------------------------------
& $Dedupc init $repo
Check "init" ($LASTEXITCODE -eq 0)

& $Dedupc ingest $repo $data
Check "ingest" ($LASTEXITCODE -eq 0)

& $Dedupc list $repo
& $Dedupc stats $repo
Check "stats" ($LASTEXITCODE -eq 0)

# --- restore each file and compare hashes ---------------------------------
foreach ($f in (Get-ChildItem $data -Recurse -File)) {
    $outDir = Join-Path $restored ([guid]::NewGuid().ToString("N").Substring(0, 8))
    & $Dedupc restore $repo --file $f.FullName --out $outDir
    $ok = $LASTEXITCODE -eq 0
    $restoredFile = Join-Path $outDir $f.Name
    if ($ok -and (Test-Path $restoredFile)) {
        if ($f.Length -eq 0) {
            $ok = (Get-Item $restoredFile).Length -eq 0
        } else {
            $h1 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash
            $h2 = (Get-FileHash $restoredFile -Algorithm SHA256).Hash
            $ok = $h1 -eq $h2
        }
    } else { $ok = $false }
    Check "roundtrip $($f.Name)" $ok
}

# --- dedup effectiveness: duplicate must not grow the store ---------------
$chunkBytes = (Get-ChildItem (Join-Path $repo "chunks") -Recurse -File |
    Measure-Object -Property Length -Sum).Sum
$rawBytes = (Get-ChildItem $data -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("store: {0:N0} bytes for {1:N0} raw bytes" -f $chunkBytes, $rawBytes)
Check "dedup+compression shrank the store" ($chunkBytes -lt ($rawBytes / 2))

# --- cleanup --------------------------------------------------------------
Remove-Item -Recurse -Force $work

if ($fail -gt 0) { Write-Host "$fail test(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "All tests passed" -ForegroundColor Green
exit 0
