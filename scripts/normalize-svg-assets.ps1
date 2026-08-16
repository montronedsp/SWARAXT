param(
    [Parameter(Mandatory = $true)] [string] $SourceRoot,
    [Parameter(Mandatory = $true)] [string] $DestinationRoot,
    [string] $NeonDestinationRoot = (Join-Path (Split-Path $DestinationRoot -Parent) 'NeonCobalt')
)

$ErrorActionPreference = 'Stop'

$assets = @{
    '01_montrone_dsp_wordmark.svg' = 'company_wordmark.svg'
    '02_swara_xt_wordmark.svg' = 'product_wordmark.svg'
    '03_instrument_square_mark.svg' = 'instrument_mark.svg'
    '04_svara_devanagari_mark.svg' = 'svara_mark.svg'
    '05_hybrid_monosynth_tagline.svg' = 'tagline.svg'
    'swara_xt_svg_graphics_library\01_separators\01_original_mutable_braid_single.svg' = 'braid.svg'
    'swara_xt_svg_graphics_library\04_frames_ornaments\01_jam_mandala_original.svg' = 'jam_ornament.svg'
    'swara_xt_svg_graphics_library\04_frames_ornaments\02_visualizer_frame_with_corner_brackets.svg' = 'visualizer_frame.svg'
    'swara_xt_svg_graphics_library\04_frames_ornaments\04_mod_matrix_frame_and_rules.svg' = 'mod_matrix_frame.svg'
}

New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null

foreach ($entry in $assets.GetEnumerator())
{
    $inputPath = Join-Path $SourceRoot $entry.Key
    $outputPath = Join-Path $DestinationRoot $entry.Value
    if (-not (Test-Path -LiteralPath $inputPath))
    {
        throw "Missing SVG source: $inputPath"
    }

    $document = [System.Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $false
    $document.Load($inputPath)
    $root = $document.DocumentElement

    foreach ($nodeName in @('metadata', 'desc', 'namedview', 'color-profile'))
    {
        @($root.SelectNodes(".//*[local-name()='$nodeName']")) | ForEach-Object {
            [void] $_.ParentNode.RemoveChild($_)
        }
    }

    $definitions = $root.SelectSingleNode("./*[local-name()='defs']")
    if ($null -ne $definitions)
    {
        $requiredIds = [System.Collections.Generic.HashSet[string]]::new()
        $scan = [System.Collections.Generic.Queue[System.Xml.XmlNode]]::new()
        @($root.ChildNodes | Where-Object { $_ -ne $definitions }) | ForEach-Object { $scan.Enqueue($_) }

        while ($scan.Count -gt 0)
        {
            $node = $scan.Dequeue()
            if ($null -ne $node.Attributes)
            {
                foreach ($attribute in $node.Attributes)
                {
                    foreach ($match in [regex]::Matches($attribute.Value, '(?:url\(#|^#)([-\w:.]+)'))
                    {
                        $id = $match.Groups[1].Value
                        if ($requiredIds.Add($id))
                        {
                            $definition = $definitions.SelectSingleNode(".//*[@id='$id']")
                            if ($null -ne $definition) { $scan.Enqueue($definition) }
                        }
                    }
                }
            }
            foreach ($child in $node.ChildNodes) { $scan.Enqueue($child) }
        }

        @($definitions.ChildNodes) | ForEach-Object {
            if ([string]::IsNullOrEmpty($_.id) -or -not $requiredIds.Contains($_.id))
            {
                [void] $definitions.RemoveChild($_)
            }
        }
        if ($definitions.ChildNodes.Count -eq 0) { [void] $root.RemoveChild($definitions) }
    }

    foreach ($element in @($root.SelectNodes('.//*')))
    {
        if ($null -eq $element.Attributes) { continue }
        foreach ($attribute in $element.Attributes)
        {
            $attribute.Value = ($attribute.Value.Replace('#d9dadb', '#e7ca83').Replace('#ed1c24', '#8e6b29'))
        }
    }

    if ($entry.Value -eq 'instrument_mark.svg')
    {
        @($document.SelectNodes('//*[@clip-path]')) | ForEach-Object {
            $_.RemoveAttribute('clip-path')
        }
        $instrumentDefs = $root.SelectSingleNode('./*[local-name()="defs"]')
        if ($null -ne $instrumentDefs) { [void] $root.RemoveChild($instrumentDefs) }
    }

    $settings = [System.Xml.XmlWriterSettings]::new()
    $settings.Indent = $true
    $settings.Encoding = [System.Text.UTF8Encoding]::new($false)
    $settings.NewLineChars = "`n"
    $settings.NewLineHandling = [System.Xml.NewLineHandling]::Replace
    $writer = [System.Xml.XmlWriter]::Create($outputPath, $settings)
    try { $document.Save($writer) } finally { $writer.Dispose() }

    if ($entry.Value -eq 'braid.svg')
    {
        @($document.SelectNodes('//*[@clip-path]')) | ForEach-Object {
            $_.RemoveAttribute('clip-path')
        }
        $braidDefs = $document.DocumentElement.SelectSingleNode('./*[local-name()="defs"]')
        if ($null -ne $braidDefs) { [void] $document.DocumentElement.RemoveChild($braidDefs) }
        $document.DocumentElement.SetAttribute('width', '7.35027')
        $document.DocumentElement.SetAttribute('height', '400.274')
        $document.DocumentElement.SetAttribute('viewBox', '33.7015 32.1545 7.35027 400.274')
        $writer = [System.Xml.XmlWriter]::Create($outputPath, $settings)
        try { $document.Save($writer) } finally { $writer.Dispose() }
    }

}

function Write-Utf8([string] $Path, [string] $Content)
{
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Get-PrefixedSvgContent([string] $Path, [string] $Prefix,
                                [hashtable] $ColourMap)
{
    $document = [System.Xml.XmlDocument]::new()
    $document.Load($Path)
    $content = $document.DocumentElement.InnerXml
    $ids = [regex]::Matches($content, 'id="([^"]+)"') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object Length -Descending -Unique
    foreach ($id in $ids)
    {
        $content = $content.Replace("id=`"$id`"", "id=`"$Prefix-$id`"")
        $content = $content.Replace("#$id", "#$Prefix-$id")
    }
    foreach ($colour in $ColourMap.GetEnumerator())
    {
        $content = $content.Replace($colour.Key, $colour.Value)
        $content = $content.Replace($colour.Key.ToUpperInvariant(), $colour.Value)
    }
    return $content
}

function Write-ProductLockup([string] $Path, [hashtable[]] $Parts)
{
    $body = [System.Text.StringBuilder]::new()
    foreach ($part in $Parts)
    {
        $content = Get-PrefixedSvgContent $part.Path $part.Prefix $part.Colours
        [void] $body.AppendLine("  <g transform=`"translate($($part.Dx) $($part.Dy))`">$content</g>")
    }
    Write-Utf8 $Path @"
<svg xmlns="http://www.w3.org/2000/svg" width="246.674" height="44.282" viewBox="0 0 246.674 44.282">
$body</svg>
"@
}

$partGeometry = @(
    @{ Name='instrument_mark.svg'; Prefix='instrument'; Dx='-93.5'; Dy='-74.5' },
    @{ Name='product_wordmark.svg'; Prefix='wordmark'; Dx='-67.808'; Dy='-72.609' },
    @{ Name='tagline.svg'; Prefix='tagline'; Dx='58.760'; Dy='-63.400' },
    @{ Name='svara_mark.svg'; Prefix='svara'; Dx='56.588'; Dy='-45.496' }
)

$midnightParts = foreach ($part in $partGeometry)
{
    @{ Path=(Join-Path $DestinationRoot $part.Name); Prefix=$part.Prefix;
       Dx=$part.Dx; Dy=$part.Dy; Colours=@{} }
}
Write-ProductLockup (Join-Path $DestinationRoot 'midnight_product_lockup.svg') $midnightParts

Write-Utf8 (Join-Path $DestinationRoot 'midnight_backdrop.svg') @'
<svg xmlns="http://www.w3.org/2000/svg" width="1113" height="521" viewBox="93.5 74.5 1113 521">
  <defs>
    <radialGradient id="violetBloom" cx="50%" cy="46%" r="62%"><stop offset="0" stop-color="#302246" stop-opacity="0.33"/><stop offset="0.58" stop-color="#171022" stop-opacity="0.11"/><stop offset="1" stop-color="#090810" stop-opacity="0"/></radialGradient>
    <pattern id="midnightTexture" width="18" height="18" patternUnits="userSpaceOnUse"><path d="M0,18 L18,0 M-9,18 L9,0 M9,18 L27,0" fill="none" stroke="#d7bf82" stroke-width="0.35" stroke-opacity="0.030"/><circle cx="4.5" cy="4.5" r="0.55" fill="#ffffff" fill-opacity="0.035"/><circle cx="13.5" cy="13.5" r="0.45" fill="#9e6ddd" fill-opacity="0.035"/></pattern>
  </defs>
  <rect x="93.5" y="74.5" width="1113" height="521" fill="url(#violetBloom)"/>
  <rect x="93.5" y="74.5" width="1113" height="521" fill="url(#midnightTexture)" opacity="0.9"/>
  <ellipse cx="350" cy="340" rx="280" ry="330" fill="#33234d" fill-opacity="0.075"/>
  <ellipse cx="1030" cy="340" rx="250" ry="330" fill="#2a203d" fill-opacity="0.060"/>
</svg>
'@

New-Item -ItemType Directory -Path $NeonDestinationRoot -Force | Out-Null
$neonColours = @(
    @{ '#e7ca83'='#f2f7ff'; '#8e6b29'='#f2f7ff' },
    @{ '#e7ca83'='#f2f7ff'; '#8e6b29'='#f2f7ff' },
    @{ '#e7ca83'='#00c9ff'; '#8e6b29'='#00c9ff' },
    @{ '#e7ca83'='#d9e8ff'; '#8e6b29'='#d9e8ff' }
)
$neonParts = for ($i = 0; $i -lt $partGeometry.Count; ++$i)
{
    $part = $partGeometry[$i]
    @{ Path=(Join-Path $DestinationRoot $part.Name); Prefix=('neon-' + $part.Prefix);
       Dx=$part.Dx; Dy=$part.Dy; Colours=$neonColours[$i] }
}
Write-ProductLockup (Join-Path $NeonDestinationRoot 'neon_product_lockup.svg') $neonParts

function Write-NeonVariant([string] $InputName, [string] $OutputName,
                           [hashtable] $Colours)
{
    $content = Get-Content -Raw -LiteralPath (Join-Path $DestinationRoot $InputName)
    foreach ($colour in $Colours.GetEnumerator())
    {
        $content = $content.Replace($colour.Key, $colour.Value)
        $content = $content.Replace($colour.Key.ToUpperInvariant(), $colour.Value)
    }
    Write-Utf8 (Join-Path $NeonDestinationRoot $OutputName) $content
}

Write-NeonVariant 'company_wordmark.svg' 'neon_company_wordmark.svg' @{ '#e7ca83'='#f2f7ff'; '#8e6b29'='#f2f7ff' }
Write-NeonVariant 'braid.svg' 'neon_braid.svg' @{ '#e7ca83'='#318fdf'; '#8e6b29'='#318fdf' }
Write-NeonVariant 'jam_ornament.svg' 'neon_jam_ornament.svg' @{ '#e7ca83'='#318fdf'; '#8e6b29'='#00c9ff' }
Write-NeonVariant 'mod_matrix_frame.svg' 'neon_mod_matrix_frame.svg' @{ '#e7ca83'='#8d54ff'; '#8e6b29'='#00c9ff' }
Write-NeonVariant 'visualizer_frame.svg' 'neon_visualizer_frame.svg' @{ '#e7ca83'='#8d54ff'; '#8e6b29'='#00c9ff' }

Write-Utf8 (Join-Path $NeonDestinationRoot 'neon_backdrop.svg') @'
<svg xmlns="http://www.w3.org/2000/svg" width="1113" height="521" viewBox="93.5 74.5 1113 521">
  <defs><pattern id="techTexture" width="24" height="24" patternUnits="userSpaceOnUse"><path d="M0,24 L24,0 M-12,24 L12,0 M12,24 L36,0" fill="none" stroke="#7bc7ff" stroke-width="0.45" stroke-opacity="0.028"/><circle cx="4" cy="4" r="0.7" fill="#43bbff" fill-opacity="0.055"/><circle cx="18" cy="8" r="0.45" fill="#43bbff" fill-opacity="0.055"/><circle cx="10" cy="19" r="0.55" fill="#43bbff" fill-opacity="0.055"/></pattern></defs>
  <rect x="93.5" y="74.5" width="1113" height="521" fill="url(#techTexture)"/>
  <ellipse cx="210" cy="245" rx="260" ry="300" fill="#008cff" fill-opacity="0.10"/>
  <ellipse cx="1120" cy="185" rx="250" ry="250" fill="#7a32ff" fill-opacity="0.12"/>
  <ellipse cx="655" cy="520" rx="420" ry="160" fill="#005ecb" fill-opacity="0.05"/>
  <path d="M102,530 L380,252" fill="none" stroke="#1fc8ff" stroke-width="1" stroke-opacity="0.07"/>
  <path d="M945,590 L1195,340" fill="none" stroke="#1fc8ff" stroke-width="1" stroke-opacity="0.08"/>
  <path d="M1000,120 L1185,120" fill="none" stroke="#1fc8ff" stroke-width="1" stroke-opacity="0.10"/>
</svg>
'@
