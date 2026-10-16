param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$BuildDirectory = "",
    [string]$OutputDirectory = ""
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
if ($version -ne "1.2.3") { throw "This release requires PROJECT_VERSION 1.2.3" }
function Assert-PackageDocumentation([string]$Directory) {
    $readme = Get-Content -Raw (Join-Path $Directory "README.txt")
    if ($readme -notmatch '^Swara XT 1\.2\.3\r?\n' -or
        $readme -notmatch '(?m)^Source repository: https://github\.com/montronedsp/SWARAXT\r?$') {
        throw "Package README version or source repository is incorrect"
    }
    foreach ($file in Get-ChildItem -LiteralPath $Directory -Recurse -File -Include *.txt,*.md) {
        if ((Get-Content -Raw -LiteralPath $file.FullName) -match 'github\.com/montronedsp/swara-xt') {
            throw "Legacy repository URL in $($file.FullName)"
        }
    }
}

$artefacts = Join-Path $BuildDirectory ("SwaraXT_artefacts/" + $Config)
$skinEditorExe = Join-Path $BuildDirectory ("SwaraXtSkinEditor_artefacts/" + $Config + "/SWARA XT Skin Editor.exe")
$skinExamples = Join-Path $repoRoot "resources/Skin/Examples"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot "artifacts"
}
$artifactsDir = [IO.Path]::GetFullPath($OutputDirectory)
$workspace = Join-Path $artifactsDir "windows-package"
$stageRoot = Join-Path $workspace "Swara XT"
$packageName = "SwaraXT-Windows-x64-v$version.zip"
$packagePath = Join-Path $artifactsDir $packageName
if ($packageName -ne "SwaraXT-Windows-x64-v1.2.3.zip") { throw "Invalid package filename" }

if (-not (Test-Path (Join-Path $artefacts "VST3/Swara XT.vst3"))) {
    throw "Release VST3 not found under $artefacts"
}
if (-not (Test-Path (Join-Path $artefacts "Standalone/Swara XT.exe"))) {
    throw "Release Standalone not found under $artefacts"
}
if (-not (Test-Path -LiteralPath $skinEditorExe)) {
    throw "Release Skin Editor not found at $skinEditorExe (build SwaraXtSkinEditor)"
}
if (-not (Test-Path -LiteralPath $skinExamples)) {
    throw "Skin Examples folder missing at $skinExamples"
}
foreach ($required in @(
        "manufacturer_mark_example.svg",
        "decoration_example.svg",
        "background_example.svg",
        "background_example.png",
        "colours_example.txt",
        "README.md")) {
    if (-not (Test-Path -LiteralPath (Join-Path $skinExamples $required))) {
        throw "Missing Skin Example: $required"
    }
}

if (Test-Path -LiteralPath $stageRoot) {
    $resolvedStage = [IO.Path]::GetFullPath($stageRoot)
    $resolvedWorkspace = [IO.Path]::GetFullPath($workspace) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedStage.StartsWith($resolvedWorkspace, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe package staging path"
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "VST3") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "Standalone") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "Skin Editor") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "Documentation") | Out-Null
New-Item -ItemType Directory -Force -Path $artifactsDir | Out-Null

Copy-Item -Recurse -Force (Join-Path $artefacts "VST3/Swara XT.vst3") (Join-Path $stageRoot "VST3/Swara XT.vst3")
Copy-Item -Force (Join-Path $artefacts "Standalone/Swara XT.exe") (Join-Path $stageRoot "Standalone/Swara XT.exe")
Copy-Item -Force -LiteralPath $skinEditorExe (Join-Path $stageRoot "Skin Editor/SWARA XT Skin Editor.exe")
Copy-Item -Recurse -Force -LiteralPath $skinExamples (Join-Path $stageRoot "Skin Examples")
Copy-Item -Force (Join-Path $repoRoot "packaging/windows/README.txt") (Join-Path $stageRoot "README.txt")
Copy-Item -Force (Join-Path $repoRoot "LICENSE") (Join-Path $stageRoot "LICENSE.txt")
Copy-Item -Force (Join-Path $repoRoot "COPYRIGHT") (Join-Path $stageRoot "COPYRIGHT.txt")
Copy-Item -Force (Join-Path $repoRoot "THIRD_PARTY_NOTICES.md") (Join-Path $stageRoot "THIRD_PARTY_NOTICES.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Skin/ATTRIBUTION.md") (Join-Path $stageRoot "Documentation/PANEL_ARTWORK_ATTRIBUTION.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Skin/CC-BY-SA-3.0.txt") (Join-Path $stageRoot "Documentation/CC-BY-SA-3.0.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Fonts/OFL-1.1.txt") (Join-Path $stageRoot "Documentation/FONT_OFL-1.1.txt")
Assert-PackageDocumentation $stageRoot
if (-not (Test-Path -LiteralPath (Join-Path $stageRoot "Skin Editor/SWARA XT Skin Editor.exe"))) {
    throw "Staged Skin Editor missing"
}
if (-not (Test-Path -LiteralPath (Join-Path $stageRoot "Skin Examples/manufacturer_mark_example.svg"))) {
    throw "Staged Skin Examples incomplete"
}

if (Test-Path $packagePath) { Remove-Item -Force $packagePath }
Compress-Archive -Path $stageRoot -DestinationPath $packagePath -Force

$hash = (Get-FileHash $packagePath -Algorithm SHA256).Hash
$checksumFile = Join-Path $artifactsDir "checksums-v$version.txt"
@(
    "SHA-256 checksums for Swara XT $version",
    "Release source: $(git -C $repoRoot rev-parse HEAD)",
    "Repository: https://github.com/montronedsp/SWARAXT",
    "",
    "$hash  $packageName"
) | Set-Content -Encoding ascii $checksumFile

Write-Host "Wrote $packagePath"
Write-Host "Wrote $checksumFile"
