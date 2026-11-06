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
if ($version -ne "1.3.1") { throw "This release requires PROJECT_VERSION 1.3.1" }
function Assert-PackageDocumentation([string]$Directory) {
    $readme = Get-Content -Raw (Join-Path $Directory "README.txt")
    if ($readme -notmatch '^Swara XT 1\.3\.1\r?\n' -or
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
$skinExamples = Join-Path $repoRoot "resources/Skin/Examples"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot "artifacts"
}
$artifactsDir = [IO.Path]::GetFullPath($OutputDirectory)
$workspace = Join-Path $artifactsDir "windows-package"
$stageRoot = Join-Path $workspace "Swara XT"
$packageName = "SwaraXT-Windows-x64-v$version.zip"
$packagePath = Join-Path $artifactsDir $packageName
if ($packageName -ne "SwaraXT-Windows-x64-v1.3.1.zip") { throw "Invalid package filename" }

if (-not (Test-Path (Join-Path $artefacts "VST3/Swara XT.vst3"))) {
    throw "Release VST3 not found under $artefacts"
}
if (-not (Test-Path (Join-Path $artefacts "Standalone/Swara XT.exe"))) {
    throw "Release Standalone not found under $artefacts"
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
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "Documentation") | Out-Null
New-Item -ItemType Directory -Force -Path $artifactsDir | Out-Null

Copy-Item -Recurse -Force (Join-Path $artefacts "VST3/Swara XT.vst3") (Join-Path $stageRoot "VST3/Swara XT.vst3")
Copy-Item -Force (Join-Path $artefacts "Standalone/Swara XT.exe") (Join-Path $stageRoot "Standalone/Swara XT.exe")
Copy-Item -Recurse -Force -LiteralPath $skinExamples (Join-Path $stageRoot "Skin Examples")
Copy-Item -Recurse -Force (Join-Path $repoRoot "resources/Skin/Decorations") (Join-Path $stageRoot "Skin Examples/Decorations")
Copy-Item (Join-Path $repoRoot "resources/Skin/ATTRIBUTION.md") (Join-Path $stageRoot "Skin Examples/ATTRIBUTION.md")
Copy-Item (Join-Path $repoRoot "resources/Skin/CC-BY-SA-3.0.txt") (Join-Path $stageRoot "Skin Examples/CC-BY-SA-3.0.txt")
Copy-Item -Force (Join-Path $repoRoot "packaging/windows/README.txt") (Join-Path $stageRoot "README.txt")
Copy-Item -Force (Join-Path $repoRoot "LICENSE") (Join-Path $stageRoot "LICENSE.txt")
Copy-Item -Force (Join-Path $repoRoot "COPYRIGHT") (Join-Path $stageRoot "COPYRIGHT.txt")
Copy-Item -Force (Join-Path $repoRoot "THIRD_PARTY_NOTICES.md") (Join-Path $stageRoot "THIRD_PARTY_NOTICES.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Skin/ATTRIBUTION.md") (Join-Path $stageRoot "Documentation/PANEL_ARTWORK_ATTRIBUTION.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Skin/CC-BY-SA-3.0.txt") (Join-Path $stageRoot "Documentation/CC-BY-SA-3.0.txt")
Copy-Item -Force (Join-Path $repoRoot "resources/Fonts/OFL-1.1.txt") (Join-Path $stageRoot "Documentation/FONT_OFL-1.1.txt")
Copy-Item (Join-Path $repoRoot "packaging/SOURCE.txt") (Join-Path $stageRoot "SOURCE.txt")
$sourceRevision = git -C $repoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw "Cannot identify package source revision" }
Add-Content -LiteralPath (Join-Path $stageRoot "SOURCE.txt") -Value "`nPackage source revision: $sourceRevision`nPackage source URL: https://github.com/montronedsp/SWARAXT/tree/$sourceRevision"
Assert-PackageDocumentation $stageRoot
if (-not (Test-Path -LiteralPath (Join-Path $stageRoot "Skin Examples/manufacturer_mark_example.svg"))) {
    throw "Staged Skin Examples incomplete"
}

if (Get-ChildItem -LiteralPath $stageRoot -Recurse -Force | Where-Object { $_.Name -match '(?i)skin[ _-]*editor' }) {
    throw "Separate Skin Editor application is not part of the release package"
}
foreach ($required in @("VST3/Swara XT.vst3", "Standalone/Swara XT.exe", "SOURCE.txt", "LICENSE.txt", "COPYRIGHT.txt", "THIRD_PARTY_NOTICES.txt", "Documentation/PANEL_ARTWORK_ATTRIBUTION.txt", "Documentation/CC-BY-SA-3.0.txt", "Documentation/FONT_OFL-1.1.txt", "Skin Examples/ATTRIBUTION.md", "Skin Examples/CC-BY-SA-3.0.txt")) {
    if (-not (Test-Path -LiteralPath (Join-Path $stageRoot $required))) { throw "Missing staged content: $required" }
}
foreach ($decoration in @("pcb_trace_separator", "greek_key_1turn", "greek_key_4turns", "roman_plait", "roman_intertwined", "roman_guilloche", "roman_interlace", "roman_wicker", "roman_meander", "roman_key_border")) {
    if (-not (Test-Path -LiteralPath (Join-Path $stageRoot "Skin Examples/Decorations/$decoration.svg"))) { throw "Missing decoration: $decoration" }
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
