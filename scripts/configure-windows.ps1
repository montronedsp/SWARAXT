param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot ("build-" + $Config.ToLowerInvariant())
}

cmake -S $repoRoot -B $BuildDirectory "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
