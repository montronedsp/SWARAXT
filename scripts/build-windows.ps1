param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string[]]$Target = @("SwaraXT_VST3", "SwaraXT_Standalone"),
    [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot ("build-" + $Config.ToLowerInvariant())
}

cmake --build $BuildDirectory --config $Config --target @Target
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
