param([Parameter(Mandatory = $true)][string]$OriginalDir)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')
$root = Assert-KqPlainPath ([IO.Path]::GetFullPath($OriginalDir))
if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'Client directory is missing.' }
if (Test-KqClientRunning $root) { throw 'Close this client before uninstalling its extension entry.' }
$runtime = Get-KqChildPath $root 'KQPetRuntime'
$lock = Enter-KqDeploymentLock $runtime
try {
    if (Test-KqClientRunning $root) { throw 'The client started; uninstall was not performed.' }
    foreach ($name in @('KQPetLauncher.exe', 'KQPetInventory.dll', 'KQPetInventory.pending.dll')) {
        $path = Get-KqChildPath $root $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
            Write-Host "Removed extension entry: $name"
        }
    }
} finally { $lock.Dispose() }
Write-Host 'Extension entry removed. Original executables, personal data, immutable releases and rollback backups were retained.'
