param(
    [Parameter(Mandatory = $true)]
    [string]$HtmlPath,
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $projectRoot 'assets\pet-image-urls.json'
}

$html = Get-Content -Raw -Encoding UTF8 -LiteralPath $HtmlPath
$images = [ordered]@{}
foreach ($match in [regex]::Matches($html, '<img\b[^>]*>', 'IgnoreCase')) {
    $tag = [System.Net.WebUtility]::HtmlDecode($match.Value)
    $nameMatch = [regex]::Match($tag, '\balt=["'']([^"'']+)["'']', 'IgnoreCase')
    $urlMatch = [regex]::Match(
        $tag,
        'https?://img[0-9]*\.a0bi\.com/[^&"''\s>]+',
        'IgnoreCase'
    )
    if (-not $nameMatch.Success -or -not $urlMatch.Success) { continue }
    $name = [System.Net.WebUtility]::HtmlDecode($nameMatch.Groups[1].Value).Trim()
    if (-not [string]::IsNullOrWhiteSpace($name)) {
        $images[$name] = $urlMatch.Value
    }
}

if ($images.Count -lt 100) {
    throw "Only $($images.Count) pet images were found; refusing to overwrite the catalog."
}

$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$json = $images | ConvertTo-Json -Compress
[System.IO.File]::WriteAllText($OutputPath, $json, [System.Text.UTF8Encoding]::new($false))
Write-Host "Generated $($images.Count) pet image URLs: $OutputPath"
