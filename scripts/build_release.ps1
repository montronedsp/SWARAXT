param(
    [string]$ConfigurePreset = "win-release",
    [string]$BuildPreset = "win-release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

Write-Host "Configuring preset: $ConfigurePreset"
cmake --preset $ConfigurePreset
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Host "Building preset: $BuildPreset"
cmake --build --preset $BuildPreset
if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }

$artefacts = Join-Path $repoRoot "build\$ConfigurePreset\SwaraXT_artefacts"
Write-Host "Build complete. Artifacts under: $artefacts"
