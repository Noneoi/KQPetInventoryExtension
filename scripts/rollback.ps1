param(
    [Parameter(Mandatory = $true)][string]$OriginalDir,
    [string]$ReleaseCheck = '',
    [string]$ReleaseId = '',
    [switch]$Legacy
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')
$root = Assert-KqPlainPath ([IO.Path]::GetFullPath($OriginalDir))
$runtime = Get-KqChildPath $root 'KQPetRuntime'
$checker = Resolve-KqReleaseCheck -ReleaseCheck $ReleaseCheck
if (Test-KqClientRunning $root) { throw 'Rollback is offline. Close this client before continuing.' }
if ($Legacy) {
    if ($ReleaseId) { throw 'Select either a v2 release ID or -Legacy.' }
    $lock = Enter-KqDeploymentLock $runtime
    try {
        if (Test-KqClientRunning $root) { throw 'The client started; rollback was not committed.' }
        $backup = Get-KqChildPath $runtime 'backups\v1.4.1'
        $backupLauncher = Get-KqChildPath $backup 'KQPetLauncher.exe'
        $backupExtension = Get-KqChildPath $backup 'KQPetInventory.dll'
        $launcherPin = Open-KqReadPin $backupLauncher
        $extensionPin = $null
        try {
        $extensionPin = Open-KqReadPin $backupExtension
        $pair = [IO.File]::ReadAllText((Get-KqChildPath $backup 'pair.json')) | ConvertFrom-Json
        if ($pair.schema -ne 1 -or $pair.kind -cne 'installed-legacy-pair') { throw 'Invalid paired rollback record.' }
        $baseline = Get-KqLegacyBaseline $backupLauncher $backupExtension
        if ($pair.baselineId -cne $baseline.id) { throw 'Rollback pair does not identify its frozen baseline.' }
        foreach ($entry in @(@{name='KQPetLauncher.exe';record=$pair.launcher}, @{name='KQPetInventory.dll';record=$pair.extension})) {
            $actual = Get-KqArtifactRecord (Get-KqChildPath $backup $entry.name)
            if ($actual.sha256 -cne $entry.record.sha256 -or $actual.size -ne $entry.record.size) { throw 'Legacy paired backup is incomplete or corrupted.' }
        }
        if (Test-Path -LiteralPath (Get-KqChildPath $root 'KQPetInventory.pending.dll')) { throw 'An old pending DLL must be quarantined before restoring the legacy launcher.' }
        $currentLauncher = Get-KqChildPath $root 'KQPetLauncher.exe'
        $currentExtension = Get-KqChildPath $root 'KQPetInventory.dll'
        $samePair = (Test-Path -LiteralPath $currentLauncher) -and (Test-Path -LiteralPath $currentExtension) -and
            (Get-KqArtifactRecord $currentLauncher).sha256 -ieq $pair.launcher.sha256 -and
            (Get-KqArtifactRecord $currentExtension).sha256 -ieq $pair.extension.sha256
        if ($samePair) { Write-Host 'The frozen legacy pair is already installed.'; return }
        # A root DLL replacement is harmless only while this verified bootstrap
        # selects its DLL from an immutable release directory.
        Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', $currentLauncher) | Out-Null
        # Bootstrap ignores the root DLL: replace the DLL first, then commit the
        # old paired launcher last. Interrupted rollback remains on valid v2.
        Copy-KqVerifiedFile (Get-KqChildPath $backup 'KQPetInventory.dll') (Get-KqChildPath $root 'KQPetInventory.dll') -Replace
        Copy-KqVerifiedFile (Get-KqChildPath $backup 'KQPetLauncher.exe') (Get-KqChildPath $root 'KQPetLauncher.exe') -Replace
        } finally {
            if ($extensionPin) { $extensionPin.Dispose() }
            $launcherPin.Dispose()
        }
    } finally { $lock.Dispose() }
    Write-Host 'Restored the frozen legacy launcher/DLL pair. All v2 releases and personal data were retained.'
    exit 0
}
$lock = Enter-KqDeploymentLock $runtime
try {
    if (Test-KqClientRunning $root) { throw 'The client started; rollback was not committed.' }
    if (-not $ReleaseId) {
        $active = [IO.File]::ReadAllText((Get-KqChildPath $runtime 'active.json')) | ConvertFrom-Json
        $ReleaseId = $active.previousReleaseId
    }
    $ReleaseId = Assert-KqReleaseId $ReleaseId
    $valid = Invoke-KqReleaseCheck $checker @('--validate', $runtime, $ReleaseId)
    $manifestHash = $valid.manifestSha256
    Write-KqAtomicJson (Get-KqChildPath $runtime 'pending.json') ([ordered]@{schema=1;releaseId=$ReleaseId;manifestSha256=$manifestHash})
} finally { $lock.Dispose() }
$result = Invoke-KqReleaseCheck $checker @('--activate', $root, $ReleaseId, $manifestHash)
if ($result.releaseId -cne $ReleaseId -or $result.manifestSha256 -ine $manifestHash) { throw 'Rollback selected another release.' }
Write-Host "Activated prior verified v2 release: $ReleaseId"
