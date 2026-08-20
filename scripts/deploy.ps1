param(
    [string]$OriginalDir = '',
    [string]$BuildBin = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildBin)) {
    $BuildBin = Join-Path $projectRoot 'build-qt663\bin\Release'
}
if ([string]::IsNullOrWhiteSpace($OriginalDir)) {
    $workspaceRoot = Split-Path -Parent $projectRoot
    $candidate = Get-ChildItem -LiteralPath $workspaceRoot -Directory | Where-Object {
        Get-ChildItem -LiteralPath $_.FullName -Filter 'KQPro*.exe' -File -ErrorAction SilentlyContinue
    } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $candidate) { throw "No directory containing KQPro*.exe was found under $workspaceRoot" }
    $OriginalDir = $candidate.FullName
}

& (Join-Path $PSScriptRoot 'verify-target.ps1') -OriginalDir $OriginalDir

$launcher = Join-Path $BuildBin 'KQPetLauncher.exe'
$extension = Join-Path $BuildBin 'KQPetInventory.dll'
if (-not (Test-Path -LiteralPath $launcher) -or -not (Test-Path -LiteralPath $extension)) {
    throw "Build outputs were not found. Run build.ps1 first. Output directory: $BuildBin"
}

Copy-Item -LiteralPath $launcher -Destination (Join-Path $OriginalDir 'KQPetLauncher.exe') -Force
Copy-Item -LiteralPath $extension -Destination (Join-Path $OriginalDir 'KQPetInventory.dll') -Force

Write-Host 'Deployment complete. The original KQPro*.exe was not modified.'
Write-Host "Run: $(Join-Path $OriginalDir 'KQPetLauncher.exe')"
