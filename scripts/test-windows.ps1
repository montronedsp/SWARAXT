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

ctest --test-dir $BuildDirectory -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed." }
