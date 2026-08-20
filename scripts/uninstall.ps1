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
$launcher = Join-Path $OriginalDir 'KQPetLauncher.exe'
$extension = Join-Path $OriginalDir 'KQPetInventory.dll'

foreach ($path in @($launcher, $extension)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
        Write-Host "Removed extension file: $path"
    }
}

Write-Host 'Uninstall complete. Original files were not modified.'
