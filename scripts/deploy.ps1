param(
    [Parameter(Mandatory = $true)][string]$OriginalDir,
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [string]$ReleaseCheck = '',
    [switch]$AllowPreview,
    [ValidateSet('', 'staged', 'pending', 'backup', 'activated', 'bootstrap')][string]$FaultAt = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')
. (Join-Path $PSScriptRoot 'target-tools.ps1')
$clientRoot = Assert-KqPlainPath ([IO.Path]::GetFullPath($OriginalDir))
$packageRoot = Assert-KqPlainPath ([IO.Path]::GetFullPath($PackageDirectory))
$package = [IO.File]::ReadAllText((Get-KqChildPath $packageRoot 'package.json')) | ConvertFrom-Json
if ($package.schema -ne 1 -or $package.bootstrapProtocol -ne 1) { throw 'Unsupported package/bootstrap schema.' }
if ($package.preview -eq $true -and -not $AllowPreview) { throw 'Preview packages require -AllowPreview and an isolated test client directory.' }
$releaseId = Assert-KqReleaseId $package.releaseId
if ($package.manifestSha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid manifest digest.' }
$checker = Resolve-KqReleaseCheck -ReleaseCheck $ReleaseCheck -BuildBin (Join-Path $packageRoot 'tools')
$sourceRuntime = Get-KqChildPath $packageRoot 'KQPetRuntime'
$sourceRelease = Get-KqChildPath $sourceRuntime ('releases\' + $releaseId)
Invoke-KqReleaseCheck $checker @('--validate', $sourceRuntime, $releaseId, $package.manifestSha256) | Out-Null
$sourceBootstrap = Get-KqChildPath $packageRoot 'KQPetLauncher.exe'
$sourceBootstrapPin = Open-KqReadPin $sourceBootstrap
try {
$bootstrapHash = Get-KqArtifactRecord $sourceBootstrap
if ($bootstrapHash.sha256 -cne $package.bootstrap.sha256 -or $bootstrapHash.size -ne $package.bootstrap.size) { throw 'Bootstrap package hash mismatch.' }
Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', $sourceBootstrap) | Out-Null
$target = Get-KqTarget -OriginalDir $clientRoot -CompatibilityCheck (Join-Path $packageRoot 'tools\KQPetCompatibilityCheck.exe')
$verified = Test-KqTarget -Target $target -CompatibilityCheck (Join-Path $packageRoot 'tools\KQPetCompatibilityCheck.exe') -ExtensionPath (Join-Path $sourceRelease 'KQPetInventory.dll')
$manifest = [IO.File]::ReadAllText((Join-Path $sourceRelease 'manifest.json')) | ConvertFrom-Json
if ($manifest.identity.profileSha256 -ine $verified.profileDigest) { throw 'Package compatibility tools and manifest profiles differ.' }
$originalHash = Get-KqArtifactRecord $target.Path
$runtime = Get-KqChildPath $clientRoot 'KQPetRuntime'
$release = Get-KqChildPath $runtime ('releases\' + $releaseId)
$bootstrapTarget = Get-KqChildPath $clientRoot 'KQPetLauncher.exe'
$legacyDll = Get-KqChildPath $clientRoot 'KQPetInventory.dll'
$pendingLegacy = Get-KqChildPath $clientRoot 'KQPetInventory.pending.dll'
$lock = Enter-KqDeploymentLock $runtime
$running = $false
try {
    if (Test-Path -LiteralPath $release) {
        Invoke-KqReleaseCheck $checker @('--validate', $runtime, $releaseId, $package.manifestSha256) | Out-Null
    } else {
        $stagingRoot = Get-KqChildPath $runtime ('staging\' + [guid]::NewGuid().ToString('N'))
        $stagedRelease = Get-KqChildPath $stagingRoot ('releases\' + $releaseId)
        [IO.Directory]::CreateDirectory($stagedRelease) | Out-Null
        foreach ($name in @('KQPetLauncher.exe', 'KQPetInventory.dll', 'manifest.json', 'test-report.json')) {
            Copy-KqVerifiedFile (Get-KqChildPath $sourceRelease $name) (Get-KqChildPath $stagedRelease $name)
        }
        Invoke-KqReleaseCheck $checker @('--validate', $stagingRoot, $releaseId, $package.manifestSha256) | Out-Null
        [IO.Directory]::CreateDirectory((Get-KqChildPath $runtime 'releases')) | Out-Null
        # Both absolute targets were checked under the same runtime before this rename.
        [IO.Directory]::Move((Get-KqChildPath $stagingRoot ('releases\' + $releaseId)), (Get-KqChildPath $runtime ('releases\' + $releaseId)))
    }
    if ($FaultAt -eq 'staged') { throw 'Injected interruption after immutable release staging.' }
    Write-KqAtomicJson (Get-KqChildPath $runtime 'pending.json') ([ordered]@{
        schema = 1; releaseId = $releaseId; manifestSha256 = $package.manifestSha256
    })
    if ($FaultAt -eq 'pending') { throw 'Injected interruption after pending intent.' }
    $running = Test-KqClientRunning $clientRoot
    if (-not $running) {
        $stable = $false
        if (Test-Path -LiteralPath $bootstrapTarget) {
            $inspection = Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', $bootstrapTarget) -AllowFailure
            $stable = $inspection.ExitCode -eq 0
        }
        if (-not $stable -and (Test-Path -LiteralPath $bootstrapTarget)) {
            if (-not (Test-Path -LiteralPath $legacyDll)) { throw 'The existing legacy launcher has no paired DLL; refusing to overwrite it.' }
            $legacyLauncherPin = Open-KqReadPin $bootstrapTarget
            $legacyExtensionPin = $null
            try {
            $legacyExtensionPin = Open-KqReadPin $legacyDll
            $baseline = Get-KqLegacyBaseline $bootstrapTarget $legacyDll
            $backup = Get-KqChildPath $runtime 'backups\v1.4.1'
            [IO.Directory]::CreateDirectory($backup) | Out-Null
            Copy-KqVerifiedFile $bootstrapTarget (Get-KqChildPath $backup 'KQPetLauncher.exe')
            Copy-KqVerifiedFile $legacyDll (Get-KqChildPath $backup 'KQPetInventory.dll')
            $pair = [ordered]@{ schema = 1; kind = 'installed-legacy-pair'
                baselineId = $baseline.id
                launcher = Get-KqArtifactRecord (Join-Path $backup 'KQPetLauncher.exe') 'KQPetLauncher.exe'
                extension = Get-KqArtifactRecord (Join-Path $backup 'KQPetInventory.dll') 'KQPetInventory.dll'
            }
            $pairPath = Get-KqChildPath $backup 'pair.json'
            if (Test-Path -LiteralPath $pairPath) {
                $existingPair = [IO.File]::ReadAllText($pairPath) | ConvertFrom-Json
                if (($existingPair | ConvertTo-Json -Depth 20 -Compress) -cne ($pair | ConvertTo-Json -Depth 20 -Compress)) { throw 'The original paired backup differs; it will not be overwritten.' }
            } else { Write-KqAtomicJson $pairPath $pair }
            } finally {
                if ($legacyExtensionPin) { $legacyExtensionPin.Dispose() }
                $legacyLauncherPin.Dispose()
            }
        }
        if (Test-Path -LiteralPath $pendingLegacy) {
            $quarantine = Get-KqChildPath $runtime 'backups\legacy-pending'
            [IO.Directory]::CreateDirectory($quarantine) | Out-Null
            $hash = (Get-KqArtifactRecord $pendingLegacy).sha256
            $destination = Get-KqChildPath $quarantine ($hash + '.untrusted.dll')
            Copy-KqVerifiedFile $pendingLegacy $destination
            # The old pending DLL is preserved, never used as a v2 release.
            Remove-Item -LiteralPath (Get-KqChildPath $clientRoot 'KQPetInventory.pending.dll') -Force
        }
    }
    if ($FaultAt -eq 'backup') { throw 'Injected interruption after paired backup.' }
} finally { $lock.Dispose() }
if ($running) {
    Write-Host 'The client is running. The full release and pending intent were staged; the active release and launcher were not changed.'
    Write-Host 'After closing the client, run this deployment command again to activate. Normal launch continues using the verified active release.'
    exit 0
}
$activation = Invoke-KqReleaseCheck $checker @('--activate', $clientRoot, $releaseId, $package.manifestSha256)
if ($activation.releaseId -cne $releaseId -or $activation.manifestSha256 -ine $package.manifestSha256) { throw 'Activation did not select the requested release.' }
if ($FaultAt -eq 'activated') { throw 'Injected interruption after active commit.' }
$lock = Enter-KqDeploymentLock $runtime
try {
    $stable = $false
    if (Test-Path -LiteralPath $bootstrapTarget) {
        $inspection = Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', $bootstrapTarget) -AllowFailure
        $stable = $inspection.ExitCode -eq 0
    }
    if (-not $stable) {
        if (Test-KqClientRunning $clientRoot) { throw 'The client started before first bootstrap installation. Close it and repeat deployment.' }
        if (Test-Path -LiteralPath $bootstrapTarget) { Get-KqLegacyBaseline $bootstrapTarget $legacyDll | Out-Null }
        Copy-KqVerifiedFile $sourceBootstrap $bootstrapTarget -Replace
        $installedHash = Get-KqArtifactRecord $bootstrapTarget
        if ($installedHash.sha256 -ine $package.bootstrap.sha256 -or $installedHash.size -ne $package.bootstrap.size) {
            throw 'Installed bootstrap differs from the package manifest.'
        }
    }
    # Stable protocol-1 entry is not replaced on each version update.
    Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', $bootstrapTarget) | Out-Null
    if ($FaultAt -eq 'bootstrap') { throw 'Injected interruption after stable entry installation.' }
} finally { $lock.Dispose() }
if ((Get-KqArtifactRecord $target.Path).sha256 -cne $originalHash.sha256) { throw 'Original client executable changed during deployment.' }
Write-Host "Activated verified release: $releaseId"
Write-Host "Launch: $bootstrapTarget"
Write-Host 'Original executable and personal data were not modified.'
} finally { $sourceBootstrapPin.Dispose() }
