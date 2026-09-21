param([Parameter(Mandatory=$true)][string]$BuildBin)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'release-tools.ps1')
function Assert-True([bool]$Value, [string]$Message) { if (-not $Value) { throw "FAIL: $Message" } }
function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    Assert-True $failed $Message
}
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$fixtureRoot = Get-KqChildPath $temporaryRoot ('kqpet-release-smoke-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
$process = $null; $readyEvent = $null; $stopEvent = $null
try {
    $bin = Assert-KqPlainPath ([IO.Path]::GetFullPath($BuildBin))
    $checker = Join-Path $bin 'KQPetReleaseCheck.exe'
    $identity = Invoke-KqReleaseCheck $checker @('--identity', (Join-Path $bin 'KQPetLauncher.exe'))
    $other = Invoke-KqReleaseCheck $checker @('--identity', (Join-Path $bin 'KQPetInventory.dll'))
    Assert-True (($identity | ConvertTo-Json -Depth 20 -Compress) -ceq ($other | ConvertTo-Json -Depth 20 -Compress)) 'fixture requires one current built artifact pair'
    $releaseId = $identity.releaseId
    $packageRoot = Get-KqChildPath $fixtureRoot 'package'
    $sourceRuntime = Get-KqChildPath $packageRoot 'KQPetRuntime'
    $sourceRelease = Get-KqChildPath $sourceRuntime ('releases\' + $releaseId)
    $toolsDir = Get-KqChildPath $packageRoot 'tools'
    $scriptsDir = Get-KqChildPath $fixtureRoot 'scripts'
    [IO.Directory]::CreateDirectory($sourceRelease) | Out-Null
    [IO.Directory]::CreateDirectory($toolsDir) | Out-Null
    [IO.Directory]::CreateDirectory($scriptsDir) | Out-Null
    foreach ($name in @('KQPetLauncher.exe','KQPetInventory.dll')) { Copy-KqVerifiedFile (Join-Path $bin $name) (Join-Path $sourceRelease $name) }
    foreach ($name in @('KQPetReleaseCheck.exe','KQPetCompatibilityCheck.exe')) { Copy-KqVerifiedFile (Join-Path $bin $name) (Join-Path $toolsDir $name) }
    Copy-KqVerifiedFile (Join-Path $bin 'KQPetBootstrap.exe') (Join-Path $packageRoot 'KQPetLauncher.exe')
    # This passing-shaped report is synthetic input for verifier tests only.
    # It is confined to this temporary fixture and is never a release report.
    $loader = Get-KqArtifactRecord (Join-Path $sourceRelease 'KQPetLauncher.exe')
    $extension = Get-KqArtifactRecord (Join-Path $sourceRelease 'KQPetInventory.dll')
    Write-KqAtomicJson (Join-Path $sourceRelease 'test-report.json') ([ordered]@{
        schema=1; releaseId=$releaseId; sourceSha256=$identity.sourceSha256; profileSha256=$identity.profileSha256
        artifacts=[ordered]@{loader=$loader;extension=$extension};suite=[ordered]@{total=1;passed=1;failed=0}
        evidence=@('isolated-verifier-fixture-not-a-real-test-report')
    })
    Write-KqAtomicJson (Join-Path $sourceRelease 'manifest.json') ([ordered]@{
        schema=1;releaseId=$releaseId;identity=$identity
        artifacts=[ordered]@{
            loader=Get-KqArtifactRecord (Join-Path $sourceRelease 'KQPetLauncher.exe') 'KQPetLauncher.exe'
            extension=Get-KqArtifactRecord (Join-Path $sourceRelease 'KQPetInventory.dll') 'KQPetInventory.dll'
        }
        tests=Get-KqArtifactRecord (Join-Path $sourceRelease 'test-report.json') 'test-report.json'
    })
    $package = [ordered]@{schema=1;releaseId=$releaseId;preview=$true;bootstrapProtocol=1
        manifestSha256=(Get-KqArtifactRecord (Join-Path $sourceRelease 'manifest.json')).sha256
        bootstrap=Get-KqArtifactRecord (Join-Path $packageRoot 'KQPetLauncher.exe') 'KQPetLauncher.exe'}
    Write-KqAtomicJson (Join-Path $packageRoot 'package.json') $package
    foreach ($name in @('deploy.ps1','rollback.ps1','release-tools.ps1')) {
        Copy-KqVerifiedFile (Join-Path (Split-Path -Parent $PSScriptRoot) $name) (Join-Path $scriptsDir $name)
    }
    # Stub only compatibility discovery in the copied fixture scripts. The real
    # Compatibility core has separate PE/profile tests. No client is executed.
    $stub = @'
function Get-KqTarget { param([string]$OriginalDir)
    [pscustomobject]@{Path=(Join-Path $OriginalDir 'KQProV1.1.4.exe');Directory=$OriginalDir}
}
function Test-KqTarget { param($Target,[string]$CompatibilityCheck)
    [pscustomobject]@{profileDigest='PROFILE_DIGEST'}
}
'@
    [IO.File]::WriteAllText((Join-Path $scriptsDir 'target-tools.ps1'), $stub.Replace('PROFILE_DIGEST',$identity.profileSha256))
    $oldRoot = Get-KqChildPath $fixtureRoot 'approved-old'
    [IO.Directory]::CreateDirectory($oldRoot) | Out-Null
    [IO.File]::WriteAllText((Join-Path $oldRoot 'KQPetLauncher.exe'), 'synthetic frozen legacy launcher')
    [IO.File]::WriteAllText((Join-Path $oldRoot 'KQPetInventory.dll'), 'synthetic frozen legacy DLL')
    Write-KqAtomicJson (Join-Path $scriptsDir 'legacy-baselines.json') ([ordered]@{schema=1;baselines=@(
        [ordered]@{id='isolated-baseline';version='test';launcher=Get-KqArtifactRecord (Join-Path $oldRoot 'KQPetLauncher.exe')
            extension=Get-KqArtifactRecord (Join-Path $oldRoot 'KQPetInventory.dll')}
    )})
    function New-Client([string]$Name) {
        $client = Get-KqChildPath $fixtureRoot $Name
        [IO.Directory]::CreateDirectory($client) | Out-Null
        foreach ($file in @('KQPetLauncher.exe','KQPetInventory.dll')) { Copy-KqVerifiedFile (Join-Path $oldRoot $file) (Join-Path $client $file) }
        [IO.File]::WriteAllText((Join-Path $client 'KQProV1.1.4.exe'), 'synthetic original - never executed')
        return $client
    }
    $deploy = Join-Path $scriptsDir 'deploy.ps1'
    $rollback = Join-Path $scriptsDir 'rollback.ps1'
    foreach ($cut in @('staged','pending','backup','activated','bootstrap')) {
        $client = New-Client ('cut-' + $cut)
        $original = Get-KqArtifactRecord (Join-Path $client 'KQProV1.1.4.exe')
        [IO.File]::WriteAllText((Join-Path $client 'KQPetInventory.pending.dll'), 'untrusted old single pending DLL')
        Assert-Throws { & $deploy -OriginalDir $client -PackageDirectory $packageRoot -AllowPreview -FaultAt $cut } "cut $cut was not reached"
        if ($cut -ne 'bootstrap') {
            Assert-True ((Get-KqArtifactRecord (Join-Path $client 'KQPetLauncher.exe')).sha256 -ceq (Get-KqArtifactRecord (Join-Path $oldRoot 'KQPetLauncher.exe')).sha256) 'interrupted first install damaged old entry'
        }
        & $deploy -OriginalDir $client -PackageDirectory $packageRoot -AllowPreview
        $resolved = Invoke-KqReleaseCheck $checker @('--resolve',$client)
        Assert-True ($resolved.releaseId -ceq $releaseId) 'resumed deploy did not select exact release'
        Assert-True ((Get-KqArtifactRecord (Join-Path $client 'KQProV1.1.4.exe')).sha256 -ceq $original.sha256) 'original changed'
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $client 'KQPetInventory.pending.dll'))) 'legacy pending was not quarantined'
    }
    $client = New-Client 'rollback'
    & $deploy -OriginalDir $client -PackageDirectory $packageRoot -AllowPreview
    $stableHash = (Get-KqArtifactRecord (Join-Path $client 'KQPetLauncher.exe')).sha256
    [IO.File]::WriteAllText((Join-Path $client 'KQPetInventory.dll'), 'ignored root DLL while stable entry is active')
    $pin = Open-KqReadPin (Join-Path $client 'KQPetLauncher.exe')
    try {
        Assert-Throws { & $rollback -OriginalDir $client -ReleaseCheck $checker -Legacy } 'locked entry rollback did not fail'
        Assert-True ((Get-KqArtifactRecord (Join-Path $client 'KQPetLauncher.exe')).sha256 -ceq $stableHash) 'failed rollback damaged stable entry'
        Invoke-KqReleaseCheck $checker @('--resolve',$client) | Out-Null
    } finally { $pin.Dispose() }
    & $rollback -OriginalDir $client -ReleaseCheck $checker -Legacy
    & $rollback -OriginalDir $client -ReleaseCheck $checker -Legacy
    foreach ($name in @('KQPetLauncher.exe','KQPetInventory.dll')) {
        Assert-True ((Get-KqArtifactRecord (Join-Path $client $name)).sha256 -ceq (Get-KqArtifactRecord (Join-Path $oldRoot $name)).sha256) 'legacy rollback lost pair'
    }
    [IO.File]::WriteAllText((Join-Path $client 'KQPetInventory.dll'), 'unknown root DLL')
    $unknownHash = (Get-KqArtifactRecord (Join-Path $client 'KQPetInventory.dll')).sha256
    Assert-Throws { & $rollback -OriginalDir $client -ReleaseCheck $checker -Legacy } 'unknown current entry accepted two-file rollback'
    Assert-True ((Get-KqArtifactRecord (Join-Path $client 'KQPetInventory.dll')).sha256 -ceq $unknownHash) 'rejected rollback changed current DLL'
    Assert-Throws { & $deploy -OriginalDir $client -PackageDirectory $packageRoot -AllowPreview } 'unknown installed pair was relabeled as baseline'

    $client = New-Client 'running'
    Copy-KqVerifiedFile (Join-Path $bin 'KQReleaseProbeChild.exe') (Join-Path $client 'KQProV1.1.4.exe') -Replace
    $nonce = [guid]::NewGuid().ToString('N')
    $readyName = 'Local\KQReleaseFixture-ready-' + $nonce
    $stopName = 'Local\KQReleaseFixture-stop-' + $nonce
    $readyEvent = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$readyName)
    $stopEvent = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$stopName)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $client 'KQProV1.1.4.exe'
    $start.Arguments = (ConvertTo-KqWindowsArgument $readyName) + ' ' + (ConvertTo-KqWindowsArgument $stopName)
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    $process = [Diagnostics.Process]::Start($start)
    Assert-True ($readyEvent.WaitOne(5000)) 'harmless running fixture did not start'
    & $deploy -OriginalDir $client -PackageDirectory $packageRoot -AllowPreview
    Assert-True (Test-Path -LiteralPath (Join-Path $client 'KQPetRuntime\pending.json')) 'running client did not stage pending'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $client 'KQPetRuntime\active.json'))) 'running client was activated'
    Assert-True ((Get-KqArtifactRecord (Join-Path $client 'KQPetLauncher.exe')).sha256 -ceq (Get-KqArtifactRecord (Join-Path $oldRoot 'KQPetLauncher.exe')).sha256) 'running first install replaced entry'
    $stopEvent.Set() | Out-Null
    Assert-True ($process.WaitForExit(5000) -and $process.ExitCode -eq 0) 'harmless fixture did not close'
    $process.Dispose(); $process=$null
    & $deploy -OriginalDir $client -PackageDirectory $packageRoot -AllowPreview

    $invalid = New-Client 'invalid'
    $originalEntry = (Get-KqArtifactRecord (Join-Path $invalid 'KQPetLauncher.exe')).sha256
    $manifestPath = Join-Path $sourceRelease 'manifest.json'
    $savedManifest = [IO.File]::ReadAllBytes($manifestPath)
    try {
        [IO.File]::WriteAllText($manifestPath,'{"schema":1}')
        Assert-Throws { & $deploy -OriginalDir $invalid -PackageDirectory $packageRoot -AllowPreview } 'broken manifest was deployed'
    } finally { [IO.File]::WriteAllBytes($manifestPath,$savedManifest) }
    Assert-True ((Get-KqArtifactRecord (Join-Path $invalid 'KQPetLauncher.exe')).sha256 -ceq $originalEntry) 'rejected manifest changed entry'
    $loaderPath = Join-Path $sourceRelease 'KQPetLauncher.exe'
    $extensionPath = Join-Path $sourceRelease 'KQPetInventory.dll'
    $reportPath = Join-Path $sourceRelease 'test-report.json'
    $savedLoader = [IO.File]::ReadAllBytes($loaderPath)
    $savedExtension = [IO.File]::ReadAllBytes($extensionPath)
    $savedReport = [IO.File]::ReadAllBytes($reportPath)
    try {
        [IO.File]::WriteAllBytes($loaderPath,$savedExtension)
        [IO.File]::WriteAllBytes($extensionPath,$savedLoader)
        $swappedReport = [IO.File]::ReadAllText($reportPath) | ConvertFrom-Json
        $swappedReport.artifacts.loader = Get-KqArtifactRecord $loaderPath
        $swappedReport.artifacts.extension = Get-KqArtifactRecord $extensionPath
        Write-KqAtomicJson $reportPath $swappedReport
        $swappedManifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
        $swappedManifest.artifacts.loader = Get-KqArtifactRecord $loaderPath 'KQPetLauncher.exe'
        $swappedManifest.artifacts.extension = Get-KqArtifactRecord $extensionPath 'KQPetInventory.dll'
        $swappedManifest.tests = Get-KqArtifactRecord $reportPath 'test-report.json'
        Write-KqAtomicJson $manifestPath $swappedManifest
        $swapped = Invoke-KqReleaseCheck $checker @('--validate',$sourceRuntime,$releaseId) -AllowFailure
        Assert-True ($swapped.ExitCode -ne 0 -and $swapped.Report.error.code -ceq 'invalid_pe') 'correctly hashed EXE/DLL role swap was accepted'
    } finally {
        [IO.File]::WriteAllBytes($loaderPath,$savedLoader)
        [IO.File]::WriteAllBytes($extensionPath,$savedExtension)
        [IO.File]::WriteAllBytes($reportPath,$savedReport)
        [IO.File]::WriteAllBytes($manifestPath,$savedManifest)
    }
    # Package an old BuildBin against a deliberately unrelated CMake version.
    # Its name/manifest must follow PE resources, and only explicit inputs enter
    # the ZIP. No synthetic report from this fixture leaves the temporary root.
    $fixtureProject = Get-KqChildPath $fixtureRoot 'package-project'
    $fixtureScripts = Get-KqChildPath $fixtureProject 'scripts'
    $fixtureBin = Get-KqChildPath $fixtureProject 'bin'
    $packageOutput = Get-KqChildPath $fixtureRoot 'package-output'
    foreach ($directory in @($fixtureScripts,$fixtureBin,(Join-Path $fixtureProject 'profiles'),(Join-Path $fixtureProject 'third_party\minhook'))) {
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
    $realProject = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    foreach ($script in @('package-release.ps1','release-tools.ps1','auto-update-tools.ps1','auto-update.ps1',
            'target-tools.ps1','verify-target.ps1','deploy.ps1','rollback.ps1','uninstall.ps1')) {
        Copy-KqVerifiedFile (Join-Path (Split-Path -Parent $PSScriptRoot) $script) (Join-Path $fixtureScripts $script)
    }
    foreach ($name in @('KQPetLauncher.exe','KQPetInventory.dll','KQPetBootstrap.exe','KQPetReleaseCheck.exe','KQPetCompatibilityCheck.exe')) {
        Copy-KqVerifiedFile (Join-Path $bin $name) (Join-Path $fixtureBin $name)
    }
    Copy-KqVerifiedFile (Join-Path $realProject 'profiles\legacy-baselines.json') (Join-Path $fixtureProject 'profiles\legacy-baselines.json')
    Copy-KqVerifiedFile (Join-Path $realProject 'third_party\minhook\LICENSE.txt') (Join-Path $fixtureProject 'third_party\minhook\LICENSE.txt')
    [IO.File]::WriteAllText((Join-Path $fixtureProject 'CMakeLists.txt'),'project(KQPetInventoryExtension VERSION 99.99.99)')
    foreach ($name in @('README.md','RELEASE_NOTES.md')) { [IO.File]::WriteAllText((Join-Path $fixtureProject $name),'isolated packaging fixture') }
    [IO.File]::WriteAllText((Join-Path $fixtureBin 'private-account.json'),'must not be packaged')
    [IO.File]::WriteAllText((Join-Path $fixtureBin 'private.pdb'),'must not be packaged')
    & (Join-Path $fixtureScripts 'package-release.ps1') -BuildBin $fixtureBin -TestReport $reportPath -OutputRoot $packageOutput -Preview
    $packaged = @(Get-ChildItem -LiteralPath $packageOutput -Directory)
    Assert-True ($packaged.Count -eq 1 -and $packaged[0].Name.Contains($releaseId) -and -not $packaged[0].Name.Contains('99.99.99')) 'package inferred identity from unrelated current CMake version'
    $leaked = @(Get-ChildItem -LiteralPath $packaged[0].FullName -File -Recurse | Where-Object { $_.Name -in @('private-account.json','private.pdb','KQProV1.1.4.exe') })
    Assert-True ($leaked.Count -eq 0) 'package allowlist leaked private/test/client inputs'
    foreach ($packagedScript in @(Get-ChildItem -LiteralPath (Join-Path $packaged[0].FullName 'tools') -Filter '*.ps1' -File)) {
        $scriptBytes = [IO.File]::ReadAllBytes($packagedScript.FullName)
        Assert-True ($scriptBytes.Length -gt 3 -and $scriptBytes[0] -eq 0xEF -and
            $scriptBytes[1] -eq 0xBB -and $scriptBytes[2] -eq 0xBF) ("packaged script is not UTF-8 with BOM: " + $packagedScript.Name)
    }
    Assert-Throws { & (Join-Path $fixtureScripts 'package-release.ps1') -BuildBin $fixtureBin -TestReport $reportPath -OutputRoot $packageOutput } 'final package bypassed acceptance gate'

    # Synthetic acceptance-shaped data tests only the packaging gate. These
    # booleans are NOT real-client/DPI/rollback evidence. Both the input and the
    # resulting package remain under the checked disposable fixture root.
    $acceptanceFixture = [ordered]@{
        schema=1;releaseId=$releaseId;loaderSha256=$loader.sha256;extensionSha256=$extension.sha256
        performancePassed=$false;performanceReviewed=$true;performanceBudgetPolicy='advisory'
        performanceNotes='Synthetic isolated advisory-gate fixture; no real performance or client acceptance was performed.'
        realClientPassed=$true;visualDpiPassed=$true;rollbackPassed=$true;originalExeUnchanged=$true
        fixture='isolated-advisory-gate-test-not-real-acceptance'
    }
    $acceptanceFixturePath = Get-KqChildPath $fixtureRoot 'synthetic-advisory-acceptance.json'
    Write-KqAtomicJson $acceptanceFixturePath $acceptanceFixture
    $advisoryOutput = Get-KqChildPath $fixtureRoot 'advisory-accepted-output'
    & (Join-Path $fixtureScripts 'package-release.ps1') -BuildBin $fixtureBin -TestReport $reportPath -AcceptanceReport $acceptanceFixturePath -OutputRoot $advisoryOutput
    $advisoryPackages = @(Get-ChildItem -LiteralPath $advisoryOutput -Directory)
    Assert-True ($advisoryPackages.Count -eq 1 -and $advisoryPackages[0].Name.Contains($releaseId) -and -not $advisoryPackages[0].Name.EndsWith('-preview')) 'complete advisory review was rejected or silently packaged as preview'
    $advisoryPackageRecord = [IO.File]::ReadAllText((Join-Path $advisoryPackages[0].FullName 'package.json')) | ConvertFrom-Json
    Assert-True ($advisoryPackageRecord.preview -eq $false -and $advisoryPackageRecord.releaseId -ceq $releaseId) 'advisory gate changed the package mode or binary identity'
    $copiedAcceptancePath = Join-Path $advisoryPackages[0].FullName 'acceptance-report.json'
    Assert-True ((Get-KqArtifactRecord $copiedAcceptancePath).sha256 -ceq (Get-KqArtifactRecord $acceptanceFixturePath).sha256) 'accepted advisory evidence was not retained exactly in the fixture package'
    $copiedAcceptance = [IO.File]::ReadAllText($copiedAcceptancePath) | ConvertFrom-Json
    Assert-True ($copiedAcceptance.performancePassed -eq $false -and $copiedAcceptance.performanceReviewed -eq $true -and
        $copiedAcceptance.performanceBudgetPolicy -ceq 'advisory' -and $copiedAcceptance.fixture -ceq 'isolated-advisory-gate-test-not-real-acceptance') 'advisory packaging rewrote the synthetic review as measured performance acceptance'

    $rejectedAcceptances = @(
        @{Name='missing-review';Field='performanceReviewed';Remove=$true},
        @{Name='false-review';Field='performanceReviewed';Value=$false},
        @{Name='missing-notes';Field='performanceNotes';Remove=$true},
        @{Name='blank-notes';Field='performanceNotes';Value='   '},
        @{Name='wrong-policy';Field='performanceBudgetPolicy';Value='strict'},
        @{Name='wrong-loader';Field='loaderSha256';Value=('0' * 64)},
        @{Name='wrong-extension';Field='extensionSha256';Value=('0' * 64)},
        @{Name='wrong-release';Field='releaseId';Value='another-fixture-release'},
        @{Name='missing-real-client';Field='realClientPassed';Remove=$true},
        @{Name='missing-dpi';Field='visualDpiPassed';Remove=$true},
        @{Name='failed-rollback';Field='rollbackPassed';Value=$false},
        @{Name='changed-original';Field='originalExeUnchanged';Value=$false}
    )
    foreach ($rejection in $rejectedAcceptances) {
        $candidateAcceptance = [ordered]@{}
        foreach ($field in $acceptanceFixture.Keys) { $candidateAcceptance[$field] = $acceptanceFixture[$field] }
        if ($rejection.ContainsKey('Remove') -and $rejection['Remove']) { $candidateAcceptance.Remove($rejection.Field) }
        else { $candidateAcceptance[$rejection.Field] = $rejection.Value }
        $candidatePath = Get-KqChildPath $fixtureRoot ('synthetic-acceptance-' + $rejection.Name + '.json')
        Write-KqAtomicJson $candidatePath $candidateAcceptance
        # A fresh output prevents an immutable-output collision from falsely
        # satisfying Assert-Throws when acceptance validation was bypassed.
        $rejectedOutput = Get-KqChildPath $fixtureRoot ('advisory-rejected-' + $rejection.Name)
        Assert-Throws { & (Join-Path $fixtureScripts 'package-release.ps1') -BuildBin $fixtureBin -TestReport $reportPath -AcceptanceReport $candidatePath -OutputRoot $rejectedOutput } ('advisory gate accepted ' + $rejection.Name)
        Assert-True (-not (Test-Path -LiteralPath $rejectedOutput)) ('rejected acceptance created package output: ' + $rejection.Name)
    }
    $package.releaseId='..\outside'
    Write-KqAtomicJson (Join-Path $packageRoot 'package.json') $package
    Assert-Throws { & $deploy -OriginalDir $invalid -PackageDirectory $packageRoot -AllowPreview } 'escaping release ID was accepted'
    Write-Host 'PASS: immutable release staging, first upgrade cuts, exact baseline, pinned rollback, running-only staging, advisory acceptance and invalid-package rejection.'
} finally {
    if ($stopEvent) { $stopEvent.Set() | Out-Null }
    if ($process) { $process.WaitForExit(5000) | Out-Null; $process.Dispose() }
    if ($readyEvent) { $readyEvent.Dispose() }
    if ($stopEvent) { $stopEvent.Dispose() }
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if ([IO.Path]::GetDirectoryName($resolved) -cne $temporaryRoot -or
        [IO.Path]::GetFileName($resolved) -notmatch '^kqpet-release-smoke-[0-9a-f]{32}$') { throw 'Refusing fixture cleanup outside its checked temporary root.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
