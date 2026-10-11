# Builds the dedupc engine. Usage:
#   .\build.ps1              configure (if needed) + build
#   .\build.ps1 -Test        ... then run the round-trip test
#   .\build.ps1 -Clean       delete the build folder first (full rebuild)
#   .\build.ps1 -BuildType Debug
#
# Toolchain lookup order: whatever cmake/ninja/g++ is already on PATH wins;
# otherwise the portable toolchain under $ToolsRoot (default %USERPROFILE%\tools)
# is appended to PATH for this process only.

param(
    [switch]$Test,
    [switch]$Clean,
    [ValidateSet("Release", "Debug")][string]$BuildType = "Release",
    [string]$ToolsRoot = (Join-Path $env:USERPROFILE "tools")
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# --- make sure the toolchain is reachable ---------------------------------
$portable = @(
    (Get-ChildItem -Directory (Join-Path $ToolsRoot "cmake-*") -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "bin" })
    (Join-Path $ToolsRoot "ninja")
    (Join-Path $ToolsRoot "mingw64\bin")
) | Where-Object { $_ -and (Test-Path $_) }

foreach ($tool in "cmake", "ninja", "g++") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        $env:PATH = "$($portable -join ';');$env:PATH"
        break
    }
}
foreach ($tool in "cmake", "ninja", "g++") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Error ("'$tool' not found on PATH or under $ToolsRoot. " +
            "See README.md ('Toolchain') for setup.")
    }
}

# --- configure + build ----------------------------------------------------
$build = Join-Path $root "build"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

if (-not (Test-Path (Join-Path $build "build.ninja"))) {
    cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$BuildType" `
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed" }
}

cmake --build $build
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed" }

$exe = Join-Path $build "engine\dedupc.exe"
Write-Host "`nBuilt: $exe" -ForegroundColor Green

# --- optional test --------------------------------------------------------
if ($Test) {
    powershell -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $root "scripts\roundtrip-test.ps1") -Dedupc $exe
    if ($LASTEXITCODE -ne 0) { Write-Error "Round-trip test failed" }
}
