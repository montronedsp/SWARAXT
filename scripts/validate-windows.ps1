param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $BuildDirectory = Join-Path $repoRoot ("build-" + $Config.ToLowerInvariant())
}

& (Join-Path $PSScriptRoot "configure-windows.ps1") -Config $Config -BuildDirectory $BuildDirectory -EnableTests
& (Join-Path $PSScriptRoot "build-windows.ps1") -Config $Config -BuildDirectory $BuildDirectory -Target @(
    "SwaraXT_VST3", "SwaraXT_Standalone", "SwaraXTQualificationTests"
)
& (Join-Path $PSScriptRoot "test-windows.ps1") -Config $Config -BuildDirectory $BuildDirectory
