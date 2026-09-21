param(
    [string]$BuildBin = '',
    [Parameter(Mandatory = $true)][string]$TestReport,
    [string]$AcceptanceReport = '',
    [string]$OutputRoot = '',
    [string]$ReleaseCheck = '',
    [switch]$Preview
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildBin) { $BuildBin = Join-Path $projectRoot 'build-v2\bin\Release' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $projectRoot 'dist' }
$bin = Assert-KqPlainPath ([IO.Path]::GetFullPath($BuildBin))
$checker = Resolve-KqReleaseCheck -ReleaseCheck $ReleaseCheck -BuildBin $bin
$identity = Invoke-KqReleaseCheck $checker @('--identity', (Join-Path $bin 'KQPetLauncher.exe'))
$other = Invoke-KqReleaseCheck $checker @('--identity', (Join-Path $bin 'KQPetInventory.dll'))
if (($identity | ConvertTo-Json -Depth 20 -Compress) -cne ($other | ConvertTo-Json -Depth 20 -Compress)) { throw 'Mixed loader/DLL identities cannot be packaged.' }
$releaseId = Assert-KqReleaseId $identity.releaseId
$loaderRecord = Get-KqArtifactRecord (Join-Path $bin 'KQPetLauncher.exe') 'KQPetLauncher.exe'
$extensionRecord = Get-KqArtifactRecord (Join-Path $bin 'KQPetInventory.dll') 'KQPetInventory.dll'
$test = [IO.File]::ReadAllText([IO.Path]::GetFullPath($TestReport)) | ConvertFrom-Json
if ($test.releaseId -cne $releaseId -or $test.sourceSha256 -cne $identity.sourceSha256 -or $test.profileSha256 -cne $identity.profileSha256 -or
    $test.artifacts.loader.sha256 -cne $loaderRecord.sha256 -or $test.artifacts.loader.size -ne $loaderRecord.size -or
    $test.artifacts.extension.sha256 -cne $extensionRecord.sha256 -or $test.artifacts.extension.size -ne $extensionRecord.size -or
    $test.suite.total -le 0 -or $test.suite.failed -ne 0 -or $test.suite.passed -ne $test.suite.total) { throw 'The passing test report is not bound to these artifacts.' }
if (-not $Preview) {
    if (-not $AcceptanceReport) { throw 'A final package requires -AcceptanceReport. Use -Preview only for isolated validation.' }
    $acceptance = [IO.File]::ReadAllText([IO.Path]::GetFullPath($AcceptanceReport)) | ConvertFrom-Json
    # Budget targets are advisory following the user's 2026-09-12 direction.
    # A practical review with recorded observations can accept reasonable headroom.
    $performanceAccepted = $acceptance.performancePassed -eq $true -or (
        $acceptance.performanceReviewed -eq $true -and
        $acceptance.performanceBudgetPolicy -ceq 'advisory' -and
        -not [string]::IsNullOrWhiteSpace([string]$acceptance.performanceNotes))
    if ($acceptance.schema -ne 1 -or $acceptance.releaseId -cne $releaseId -or
        $acceptance.loaderSha256 -cne $loaderRecord.sha256 -or $acceptance.extensionSha256 -cne $extensionRecord.sha256 -or
        $acceptance.realClientPassed -ne $true -or -not $performanceAccepted -or
        $acceptance.visualDpiPassed -ne $true -or $acceptance.rollbackPassed -ne $true -or $acceptance.originalExeUnchanged -ne $true) {
        throw 'Final acceptance is incomplete or belongs to other artifacts.'
    }
}
$output = Assert-KqPlainPath ([IO.Path]::GetFullPath($OutputRoot))
$suffix = if ($Preview) { '-preview' } else { '' }
$packageName = "KQPetInventory-$releaseId-win-x64$suffix"
$packageRoot = Get-KqChildPath $output $packageName
$zipPath = Get-KqChildPath $output ($packageName + '.zip')
if ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $zipPath)) { throw 'Immutable package output already exists; choose another output directory.' }
$runtime = Get-KqChildPath $packageRoot 'KQPetRuntime'
$release = Get-KqChildPath $runtime ('releases\' + $releaseId)
[IO.Directory]::CreateDirectory($release) | Out-Null
Copy-KqVerifiedFile (Join-Path $bin 'KQPetLauncher.exe') (Join-Path $release 'KQPetLauncher.exe')
Copy-KqVerifiedFile (Join-Path $bin 'KQPetInventory.dll') (Join-Path $release 'KQPetInventory.dll')
Copy-KqVerifiedFile $TestReport (Join-Path $release 'test-report.json')
$manifest = [ordered]@{ schema = 1; releaseId = $releaseId; identity = $identity
    artifacts = [ordered]@{ loader = $loaderRecord; extension = $extensionRecord }
    tests = Get-KqArtifactRecord (Join-Path $release 'test-report.json') 'test-report.json' }
Write-KqAtomicJson (Join-Path $release 'manifest.json') $manifest
Invoke-KqReleaseCheck $checker @('--validate', $runtime, $releaseId) | Out-Null
Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', (Join-Path $bin 'KQPetBootstrap.exe')) | Out-Null
Copy-KqVerifiedFile (Join-Path $bin 'KQPetBootstrap.exe') (Join-Path $packageRoot 'KQPetLauncher.exe')
$toolsDir = Get-KqChildPath $packageRoot 'tools'
[IO.Directory]::CreateDirectory($toolsDir) | Out-Null
Copy-KqVerifiedFile $checker (Join-Path $toolsDir 'KQPetReleaseCheck.exe')
Copy-KqVerifiedFile (Join-Path $bin 'KQPetCompatibilityCheck.exe') (Join-Path $toolsDir 'KQPetCompatibilityCheck.exe')
foreach ($script in @('release-tools.ps1', 'auto-update-tools.ps1', 'auto-update.ps1',
        'target-tools.ps1', 'verify-target.ps1', 'deploy.ps1', 'rollback.ps1', 'uninstall.ps1')) {
    $scriptSource = Join-Path $PSScriptRoot $script
    $scriptTarget = Join-Path $toolsDir $script
    # Windows PowerShell 5.1 treats BOM-less UTF-8 as the active ANSI codepage.
    # Always ship scripts as UTF-8 with BOM so Chinese diagnostics cannot alter
    # quote characters and make otherwise valid scripts fail to parse.
    $scriptText = [IO.File]::ReadAllText($scriptSource)
    [IO.File]::WriteAllText($scriptTarget, $scriptText, (New-Object Text.UTF8Encoding($true)))
    if ([IO.File]::ReadAllText($scriptTarget) -cne $scriptText) { throw "Packaged script verification failed: $script" }
}
Copy-KqVerifiedFile (Join-Path $projectRoot 'profiles\legacy-baselines.json') (Join-Path $toolsDir 'legacy-baselines.json')
foreach ($document in @('README.md', 'RELEASE_NOTES.md')) { Copy-KqVerifiedFile (Join-Path $projectRoot $document) (Join-Path $packageRoot $document) }
$licenses = Get-KqChildPath $packageRoot 'licenses'
[IO.Directory]::CreateDirectory($licenses) | Out-Null
Copy-KqVerifiedFile (Join-Path $projectRoot 'third_party\minhook\LICENSE.txt') (Join-Path $licenses 'MinHook-LICENSE.txt')
if ($AcceptanceReport -and -not $Preview) { Copy-KqVerifiedFile $AcceptanceReport (Join-Path $packageRoot 'acceptance-report.json') }
$package = [ordered]@{ schema = 1; releaseId = $releaseId; preview = [bool]$Preview; bootstrapProtocol = 1
    manifestSha256 = (Get-KqArtifactRecord (Join-Path $release 'manifest.json')).sha256
    bootstrap = Get-KqArtifactRecord (Join-Path $packageRoot 'KQPetLauncher.exe') 'KQPetLauncher.exe' }
Write-KqAtomicJson (Join-Path $packageRoot 'package.json') $package
# Only the allowlist above is copied: no client, account data, logs, fixtures or PDBs.
$hashLines = @(Get-ChildItem -LiteralPath $packageRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
    (Get-KqArtifactRecord $_.FullName).sha256 + '  ' + $relative
})
[IO.File]::WriteAllLines((Join-Path $packageRoot 'SHA256SUMS.txt'), $hashLines, (New-Object System.Text.UTF8Encoding($false)))
Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host "Package: $zipPath"
