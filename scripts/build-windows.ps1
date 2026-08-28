param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$BuildDirectory = "",
    [string[]]$Target = @("SwaraXT_VST3", "SwaraXT_Standalone")
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot ("build-" + $Config.ToLowerInvariant())
}

$cmakeArgs = @("--build", $BuildDirectory, "--config", $Config)
foreach ($t in $Target) {
    $cmakeArgs += @("--target", $t)
}
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }
