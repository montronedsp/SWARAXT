param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot "build/win-release"
}

$version = (Select-String -Path (Join-Path $repoRoot "CMakeLists.txt") `
    -Pattern '^project\(SwaraXT VERSION ([0-9.]+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
if ([string]::IsNullOrWhiteSpace($version)) {
    throw "Unable to read project version from CMakeLists.txt"
}

$artefacts = Join-Path $BuildDirectory ("SwaraXT_artefacts/" + $Config)
$workspace = Join-Path $repoRoot "artifacts/windows-package"
$stageRoot = Join-Path $workspace "Swara XT"
$artifactsDir = Join-Path $repoRoot "artifacts"
$packageName = "SwaraXT-Windows-x64-v$version.zip"
$packagePath = Join-Path $artifactsDir $packageName

if (-not (Test-Path (Join-Path $artefacts "VST3/Swara XT.vst3"))) {
    throw "Release VST3 not found under $artefacts"
}
if (-not (Test-Path (Join-Path $artefacts "Standalone/Swara XT.exe"))) {
    throw "Release Standalone not found under $artefacts"
}

Remove-Item -Recurse -Force $stageRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "VST3") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "Standalone") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "Documentation") | Out-Null
New-Item -ItemType Directory -Force -Path $artifactsDir | Out-Null

Copy-Item -Recurse -Force (Join-Path $artefacts "VST3/Swara XT.vst3") (Join-Path $stageRoot "VST3/Swara XT.vst3")
Copy-Item -Force (Join-Path $artefacts "Standalone/Swara XT.exe") (Join-Path $stageRoot "Standalone/Swara XT.exe")
Copy-Item -Force (Join-Path $repoRoot "packaging/windows/README.txt") (Join-Path $stageRoot "README.txt")
Copy-Item -Force (Join-Path $repoRoot "LICENSE") (Join-Path $stageRoot "LICENSE.txt")
Copy-Item -Force (Join-Path $repoRoot "COPYRIGHT") (Join-Path $stageRoot "COPYRIGHT.txt")
Copy-Item -Force (Join-Path $repoRoot "THIRD_PARTY_NOTICES.md") (Join-Path $stageRoot "THIRD_PARTY_NOTICES.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Skin/ATTRIBUTION.md") (Join-Path $stageRoot "Documentation/PANEL_ARTWORK_ATTRIBUTION.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Skin/CC-BY-SA-3.0.txt") (Join-Path $stageRoot "Documentation/CC-BY-SA-3.0.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Fonts/OFL-1.1.txt") (Join-Path $stageRoot "Documentation/FONT_OFL-1.1.txt")

if (Test-Path $packagePath) { Remove-Item -Force $packagePath }
Compress-Archive -Path $stageRoot -DestinationPath $packagePath -Force

$hash = (Get-FileHash $packagePath -Algorithm SHA256).Hash
$checksumFile = Join-Path $artifactsDir "checksums-v$version.txt"
@(
    "SHA-256 checksums for Swara XT $version",
    "",
    "$hash  $packageName"
) | Set-Content -Encoding ascii $checksumFile

Write-Host "Wrote $packagePath"
Write-Host "Wrote $checksumFile"
