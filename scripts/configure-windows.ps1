param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$BuildDirectory = "",
    [switch]$EnableTests
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot ("build-" + $Config.ToLowerInvariant())
}

$tests = if ($EnableTests) { "ON" } else { "OFF" }
cmake -S $repoRoot -B $BuildDirectory "-DCMAKE_BUILD_TYPE=$Config" "-DSWARA_BUILD_TESTS=$tests"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
