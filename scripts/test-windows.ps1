param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$BuildDirectory = "",
    [ValidateSet("all", "quick", "extended")]
    [string]$Profile = "all"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot ("build-" + $Config.ToLowerInvariant())
}

$testArgs = @("--test-dir", $BuildDirectory, "-C", $Config, "--output-on-failure", "--no-tests=error", "--parallel", "1")
if ($Profile -ne "all") { $testArgs += @("-L", "^$Profile`$") }
& ctest @testArgs
if ($LASTEXITCODE -ne 0) { throw "Tests failed." }
