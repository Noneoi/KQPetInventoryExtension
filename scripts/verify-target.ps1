param(
    [string]$OriginalDir = ''
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($OriginalDir)) {
    $projectRoot = Split-Path -Parent $PSScriptRoot
    $workspaceRoot = Split-Path -Parent $projectRoot
    $candidate = Get-ChildItem -LiteralPath $workspaceRoot -Directory | Where-Object {
        Get-ChildItem -LiteralPath $_.FullName -Filter 'KQPro*.exe' -File -ErrorAction SilentlyContinue
    } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $candidate) { throw "No directory containing KQPro*.exe was found under $workspaceRoot" }
    $OriginalDir = $candidate.FullName
}
$targets = Get-ChildItem -LiteralPath $OriginalDir -Filter 'KQPro*.exe' -File
$ranked = foreach ($target in $targets) {
    $version = [version]'0.0'
    if ($target.BaseName -match 'V(\d+(?:\.\d+)*)') { $version = [version]$Matches[1] }
    [pscustomobject]@{ Path = $target.FullName; Version = $version }
}
$targetExePath = ($ranked | Sort-Object Version -Descending | Select-Object -First 1).Path

if (-not $targetExePath) {
    throw "No KQPro*.exe was found in: $OriginalDir"
}

$actual = (Get-FileHash -LiteralPath $targetExePath -Algorithm SHA256).Hash
Write-Host 'Original target discovery passed. Runtime signatures will be checked before hooking.'
Write-Host "Path: $targetExePath"
Write-Host "SHA-256: $actual"
