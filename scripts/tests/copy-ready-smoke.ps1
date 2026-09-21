param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [string]$WrapperScript = ''
)
$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $scriptRoot 'release-tools.ps1')

function Assert-True([bool]$Value, [string]$Message) {
    if (-not $Value) { throw "FAIL: $Message" }
}
function Capture-Failure([scriptblock]$Action, [string]$Message) {
    $failure = $null
    try { & $Action | Out-Null } catch { $failure = $_.Exception.Message }
    Assert-True ([bool]$failure) $Message
    return $failure
}

$packageSource = Assert-KqPlainPath ([IO.Path]::GetFullPath($PackageDirectory))
if (-not $WrapperScript) { $WrapperScript = Join-Path $scriptRoot 'start-copy-ready.ps1' }
$wrapperSource = Assert-KqPlainPath ([IO.Path]::GetFullPath($WrapperScript))
$wrapperText = [IO.File]::ReadAllText($wrapperSource)
$package = [IO.File]::ReadAllText((Join-Path $packageSource 'package.json')) | ConvertFrom-Json
$releaseId = Assert-KqReleaseId $package.releaseId
$sourceChecker = Join-Path $packageSource 'tools\KQPetReleaseCheck.exe'
Invoke-KqReleaseCheck $sourceChecker @('--validate',(Join-Path $packageSource 'KQPetRuntime'),$releaseId,$package.manifestSha256) | Out-Null
$sourceManifestPath = Join-Path $packageSource ('KQPetRuntime\releases\' + $releaseId + '\manifest.json')
$sourceManifest = [IO.File]::ReadAllText($sourceManifestPath) | ConvertFrom-Json
$sourceManifestHash = (Get-KqArtifactRecord $sourceManifestPath).sha256
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$fixtureRoot = Get-KqChildPath $temporaryRoot ('kqpet-copy-ready-smoke-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null

try {
    # Only public package payload is copied. None of the development/client
    # cache directories is read, and no original executable is ever started.
    $relativeFiles = @('KQPetLauncher.exe','package.json','README.md','RELEASE_NOTES.md','SHA256SUMS.txt',
        'tools\KQPetReleaseCheck.exe','tools\KQPetCompatibilityCheck.exe','tools\legacy-baselines.json',
        'licenses\MinHook-LICENSE.txt')
    foreach ($name in @('release-tools.ps1','auto-update-tools.ps1','auto-update.ps1',
            'target-tools.ps1','verify-target.ps1','deploy.ps1','rollback.ps1','uninstall.ps1')) {
        $relativeFiles += 'tools\' + $name
    }
    foreach ($name in @('KQPetLauncher.exe','KQPetInventory.dll','manifest.json','test-report.json')) {
        $relativeFiles += 'KQPetRuntime\releases\' + $releaseId + '\' + $name
    }
    $targetStub = @'
function Get-KqTarget {
    param([string]$OriginalDir, [string]$CompatibilityCheck)
    [pscustomobject]@{Path=(Join-Path $OriginalDir 'KQProSmoke.exe');Directory=$OriginalDir}
}
function Test-KqTarget {
    param($Target, [string]$CompatibilityCheck, [string]$ExtensionPath)
    [pscustomobject]@{profileDigest='PROFILE_DIGEST'}
}
'@
    $targetStub = $targetStub.Replace('PROFILE_DIGEST',$sourceManifest.identity.profileSha256)

    function New-FixtureClient([string]$Name, [switch]$Legacy, [switch]$WithoutClient) {
        $clientRoot = Get-KqChildPath $fixtureRoot $Name
        [IO.Directory]::CreateDirectory($clientRoot) | Out-Null
        if (-not $WithoutClient) {
            [IO.File]::WriteAllText((Join-Path $clientRoot 'KQProSmoke.exe'),'synthetic original executable - never executed')
        }
        $cache = Get-KqChildPath $clientRoot 'KQPetData'
        [IO.Directory]::CreateDirectory($cache) | Out-Null
        [IO.File]::WriteAllText((Join-Path $cache 'synthetic-cache-marker.txt'),'isolated fixture cache must be retained')
        if ($Legacy) {
            [IO.File]::WriteAllText((Join-Path $clientRoot 'KQPetLauncher.exe'),'synthetic approved legacy launcher')
            [IO.File]::WriteAllText((Join-Path $clientRoot 'KQPetInventory.dll'),'synthetic approved legacy DLL')
        }
        return $clientRoot
    }

    function Add-CopyReadyBundle([string]$ClientRoot, [switch]$Legacy) {
        $bundle = Get-KqChildPath $ClientRoot 'KQPetQuickStart'
        $payload = Get-KqChildPath $bundle 'package'
        [IO.Directory]::CreateDirectory($payload) | Out-Null
        foreach ($relative in $relativeFiles) {
            $target = Get-KqChildPath $payload $relative
            [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
            Copy-KqVerifiedFile (Get-KqChildPath $packageSource $relative) $target
        }
        $copiedTools = Get-KqChildPath $payload 'tools'
        # Stub only client compatibility discovery. ReleaseCheck, deployment,
        # manifests, immutable binaries and real test reports remain unchanged.
        [IO.File]::WriteAllText((Join-Path $copiedTools 'target-tools.ps1'),$targetStub,(New-Object Text.UTF8Encoding($true)))
        if ($Legacy) {
            Write-KqAtomicJson (Join-Path $copiedTools 'legacy-baselines.json') ([ordered]@{schema=1;baselines=@(
                [ordered]@{id='copy-ready-synthetic-baseline';version='isolated-test'
                    launcher=Get-KqArtifactRecord (Join-Path $ClientRoot 'KQPetLauncher.exe')
                    extension=Get-KqArtifactRecord (Join-Path $ClientRoot 'KQPetInventory.dll')}
            )})
        }
        $entry = Get-KqChildPath $bundle 'start.ps1'
        [IO.File]::WriteAllText($entry,$wrapperText,(New-Object Text.UTF8Encoding($true)))
        $bytes = [IO.File]::ReadAllBytes($entry)
        Assert-True ($bytes.Length -gt 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) 'wrapper fixture must be UTF-8 with BOM for Windows PowerShell 5.1'
        return [pscustomobject]@{Client=$ClientRoot;Bundle=$bundle;Payload=$payload;Entry=$entry;Tools=$copiedTools
            Checker=(Join-Path $copiedTools 'KQPetReleaseCheck.exe')}
    }

    function Installed-State([string]$ClientRoot) {
        $result = [ordered]@{}
        $base = (Assert-KqPlainPath $ClientRoot).TrimEnd('\') + '\'
        $bundle = (Get-KqChildPath $ClientRoot 'KQPetQuickStart').TrimEnd('\') + '\'
        foreach ($file in @(Get-ChildItem -LiteralPath $ClientRoot -Recurse -File | Sort-Object FullName)) {
            if ($file.FullName.StartsWith($bundle,[StringComparison]::OrdinalIgnoreCase)) { continue }
            $relative = $file.FullName.Substring($base.Length)
            # Deployment's zero-byte lock is a valid state file, not a PE
            # artifact; include it without the nonempty-artifact precondition.
            $digest = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $result[$relative] = [ordered]@{size=[long]$file.Length;sha256=$digest}
        }
        return ConvertTo-Json -InputObject $result -Depth 6 -Compress
    }

    $freshClient = New-FixtureClient '中文 client with spaces'
    $freshBeforeCopy = Installed-State $freshClient
    $fresh = Add-CopyReadyBundle $freshClient
    Assert-True ((Installed-State $freshClient) -ceq $freshBeforeCopy) 'copying the bundle changed original files or installed a root launcher early'
    & $fresh.Entry -PrepareOnly
    $selection = Invoke-KqReleaseCheck $fresh.Checker @('--resolve',$freshClient)
    Assert-True ($selection.releaseId -ceq $releaseId -and $selection.manifestSha256 -ieq $package.manifestSha256) 'first prepare did not activate the exact verified package'
    foreach ($packagedScript in @(Get-ChildItem -LiteralPath $fresh.Tools -Filter '*.ps1' -File)) {
        $scriptBytes = [IO.File]::ReadAllBytes($packagedScript.FullName)
        Assert-True ($scriptBytes.Length -gt 3 -and $scriptBytes[0] -eq 0xEF -and
            $scriptBytes[1] -eq 0xBB -and $scriptBytes[2] -eq 0xBF) ("copy-ready script is not UTF-8 with BOM: " + $packagedScript.Name)
    }
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $windowsPowerShell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `
        (Join-Path $fresh.Tools 'auto-update.ps1') -ClientRoot $freshClient -ReleaseCheck $fresh.Checker -FinalizePendingOnly
    Assert-True ($LASTEXITCODE -eq 0) 'Windows PowerShell 5.1 could not parse or finalize the packaged auto-update script'
    Invoke-KqReleaseCheck $fresh.Checker @('--bootstrap-protocol',(Join-Path $freshClient 'KQPetLauncher.exe')) | Out-Null
    Assert-True ([IO.File]::ReadAllText((Join-Path $freshClient 'KQProSmoke.exe')) -ceq 'synthetic original executable - never executed') 'first prepare altered the original executable'
    Assert-True ([IO.File]::ReadAllText((Join-Path $freshClient 'KQPetData\synthetic-cache-marker.txt')) -ceq 'isolated fixture cache must be retained') 'first prepare altered the synthetic cache'

    $readyState = Installed-State $freshClient
    $deployCopy = Get-KqChildPath $fresh.Tools 'deploy.ps1'
    Remove-Item -LiteralPath $deployCopy -Force
    & $fresh.Entry -PrepareOnly
    Assert-True ((Installed-State $freshClient) -ceq $readyState) 'repeat prepare redeployed or altered an already ready installation'
    Assert-True (-not (Test-Path -LiteralPath $deployCopy)) 'repeat prepare depended on recreating deployment script'
    Copy-KqVerifiedFile (Join-Path $packageSource 'tools\deploy.ps1') $deployCopy

    $copiedManifest = Get-KqChildPath $fresh.Payload ('KQPetRuntime\releases\' + $releaseId + '\manifest.json')
    $originalManifest = [IO.File]::ReadAllBytes($copiedManifest)
    $beforeRejected = Installed-State $freshClient
    try {
        [IO.File]::WriteAllText($copiedManifest,'{"schema":1}')
        $failure = Capture-Failure { & $fresh.Entry -PrepareOnly } 'corrupted manifest did not reject preparation'
        Assert-True ($failure -match 'manifest|ReleaseCheck|release') 'corrupted manifest failed for an unrelated reason'
        Assert-True ((Installed-State $freshClient) -ceq $beforeRejected) 'rejected corrupted manifest changed installed files or cache'
    } finally { [IO.File]::WriteAllBytes($copiedManifest,$originalManifest) }

    $legacyClient = New-FixtureClient 'legacy client with spaces' -Legacy
    $legacyLauncher = Get-KqArtifactRecord (Join-Path $legacyClient 'KQPetLauncher.exe')
    $legacyExtension = Get-KqArtifactRecord (Join-Path $legacyClient 'KQPetInventory.dll')
    $legacyBeforeCopy = Installed-State $legacyClient
    $legacy = Add-CopyReadyBundle $legacyClient -Legacy
    Assert-True ((Installed-State $legacyClient) -ceq $legacyBeforeCopy) 'copy phase overwrote the root legacy launcher or its DLL'
    & $legacy.Entry -PrepareOnly
    $legacySelection = Invoke-KqReleaseCheck $legacy.Checker @('--resolve',$legacyClient)
    Assert-True ($legacySelection.releaseId -ceq $releaseId) 'legacy prepare did not activate the requested release'
    $backup = Get-KqChildPath $legacyClient 'KQPetRuntime\backups\v1.4.1'
    Assert-True ((Get-KqArtifactRecord (Join-Path $backup 'KQPetLauncher.exe')).sha256 -ceq $legacyLauncher.sha256) 'legacy launcher was not backed up before first bootstrap installation'
    Assert-True ((Get-KqArtifactRecord (Join-Path $backup 'KQPetInventory.dll')).sha256 -ceq $legacyExtension.sha256) 'legacy DLL was not preserved in the paired backup'
    Assert-True ((Get-KqArtifactRecord (Join-Path $legacyClient 'KQPetInventory.dll')).sha256 -ceq $legacyExtension.sha256) 'prepare changed the preserved legacy root DLL'

    $wrongClient = New-FixtureClient 'wrong folder without original' -WithoutClient
    $wrong = Add-CopyReadyBundle $wrongClient
    $wrongBefore = Installed-State $wrongClient
    $failure = Capture-Failure { & $wrong.Entry -PrepareOnly } 'wrong-directory copy did not fail'
    Assert-True ($failure.Contains('未找到氪奇主程序') -and $failure.Contains('KQPro*.exe')) 'wrong-directory failure did not explain where to copy the bundle'
    Assert-True ((Installed-State $wrongClient) -ceq $wrongBefore -and
        -not (Test-Path -LiteralPath (Join-Path $wrongClient 'KQPetRuntime'))) 'wrong-directory attempt altered the destination'
    Assert-True ((Get-KqArtifactRecord $sourceManifestPath).sha256 -ceq $sourceManifestHash) 'test changed its input release package'
    Write-Host 'PASS: copy-ready first install, repeat without deploy, invalid manifest rejection, legacy copy/backup preservation and friendly wrong-folder failure; no client launched.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if ([IO.Path]::GetDirectoryName($resolved) -ine $temporaryRoot -or
        [IO.Path]::GetFileName($resolved) -notmatch '^kqpet-copy-ready-smoke-[0-9a-f]{32}$') {
        throw 'Refusing fixture cleanup outside its checked temporary root.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
