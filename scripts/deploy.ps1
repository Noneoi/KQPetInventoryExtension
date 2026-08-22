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
$extensionTarget = Join-Path $OriginalDir 'KQPetInventory.dll'
$pendingTarget = Join-Path $OriginalDir 'KQPetInventory.pending.dll'
$deferred = $false
try {
    Copy-Item -LiteralPath $extension -Destination $extensionTarget -Force
    if (Test-Path -LiteralPath $pendingTarget) {
        Remove-Item -LiteralPath $pendingTarget -Force
    }
} catch [System.IO.IOException] {
    Copy-Item -LiteralPath $extension -Destination $pendingTarget -Force
    $deferred = $true
}

if ($deferred) {
    Write-Host 'The original client is running. The new DLL was staged safely and will be activated by the launcher after the client is closed.'
} else {
    Write-Host 'Deployment complete.'
}
Write-Host 'The original KQPro*.exe was not modified.'
Write-Host "Run: $(Join-Path $OriginalDir 'KQPetLauncher.exe')"
