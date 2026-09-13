param([string]$BuildDirectory = '', [string]$ReleaseCheck = '', [string]$CTest = '', [string]$ReportDirectory = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'build-v2' }
$buildRoot = Assert-KqPlainPath ([IO.Path]::GetFullPath($BuildDirectory))
# Build the whole registered graph before reading identity: testing stale test
# executables against freshly rebuilt production artifacts proves nothing.
[IO.Directory]::CreateDirectory($buildRoot) | Out-Null
$buildLog = Get-KqChildPath $buildRoot 'release-validation-build.log'
& (Join-Path $PSScriptRoot 'build.ps1') -BuildDirectory $buildRoot -Configuration Release *> $buildLog
if ($LASTEXITCODE -ne 0) { throw "Complete validation build failed. Inspect $buildLog" }
$bin = Get-KqChildPath $buildRoot 'bin\Release'
$checker = Resolve-KqReleaseCheck -ReleaseCheck $ReleaseCheck -BuildBin $bin
if (-not $CTest) {
    $candidate = Get-Command ctest.exe -ErrorAction SilentlyContinue
    if ($candidate) { $CTest = $candidate.Source }
    else { $CTest = 'D:\Visual Studio\VS\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' }
}
if (-not (Test-Path -LiteralPath $CTest -PathType Leaf)) { throw 'Pass the CTest executable with -CTest.' }
$loader = Join-Path $bin 'KQPetLauncher.exe'
$extension = Join-Path $bin 'KQPetInventory.dll'
$identity = Invoke-KqReleaseCheck $checker @('--identity', $loader)
$other = Invoke-KqReleaseCheck $checker @('--identity', $extension)
if (($identity | ConvertTo-Json -Depth 20 -Compress) -cne ($other | ConvertTo-Json -Depth 20 -Compress)) { throw 'Loader and extension resource identities differ.' }
if ($identity.configuration -cne 'Release') { throw 'Release validation requires a Release build.' }
$releaseId = Assert-KqReleaseId $identity.releaseId
$inputsPath = Get-KqChildPath $buildRoot 'generated\build-input-hashes.txt'
if ((Get-KqArtifactRecord $inputsPath).sha256 -cne $identity.sourceSha256) { throw 'Build input inventory does not match the binary identity.' }
foreach ($line in [IO.File]::ReadAllLines($inputsPath)) {
    if ($line -cnotmatch '^(.+):([0-9a-f]{64})$') { throw 'Malformed build input inventory.' }
    $relative = $Matches[1]; $digest = $Matches[2]
    if ((Get-FileHash -LiteralPath (Get-KqChildPath $projectRoot $relative) -Algorithm SHA256).Hash -ine $digest) { throw "Source changed since this build: $relative" }
}
$before = [ordered]@{ loader = Get-KqArtifactRecord $loader; extension = Get-KqArtifactRecord $extension }
$testBinaries = @(Get-ChildItem -LiteralPath $bin -Filter '*.exe' -File | Sort-Object Name | ForEach-Object {
    Get-KqArtifactRecord $_.FullName $_.Name
})
if (-not $ReportDirectory) { $ReportDirectory = Get-KqChildPath $buildRoot ('validation\' + $releaseId) }
$reportRoot = Assert-KqPlainPath ([IO.Path]::GetFullPath($ReportDirectory))
if (Test-Path -LiteralPath $reportRoot) { throw 'Use a fresh report directory; a prior validation record is immutable.' }
[IO.Directory]::CreateDirectory($reportRoot) | Out-Null
$junit = Get-KqChildPath $reportRoot 'ctest-results.xml'
$log = Get-KqChildPath $reportRoot 'ctest.log'
& $CTest --test-dir $buildRoot -C Release --output-on-failure --output-junit $junit *> $log
$testExit = $LASTEXITCODE
[xml]$xml = [IO.File]::ReadAllText($junit)
$suite = $xml.testsuite
$total = [int]$suite.tests
$failed = [int]$suite.failures + [int]$suite.errors + [int]$suite.disabled + [int]$suite.skipped
if ($testExit -ne 0 -or $total -le 0 -or $failed -ne 0) { throw "CTest did not pass every test. Inspect $log" }
$after = [ordered]@{ loader = Get-KqArtifactRecord $loader; extension = Get-KqArtifactRecord $extension }
if (($before | ConvertTo-Json -Depth 20 -Compress) -cne ($after | ConvertTo-Json -Depth 20 -Compress)) { throw 'Artifacts changed during validation.' }
$testedAfter = @(Get-ChildItem -LiteralPath $bin -Filter '*.exe' -File | Sort-Object Name | ForEach-Object {
    Get-KqArtifactRecord $_.FullName $_.Name
})
if (($testBinaries | ConvertTo-Json -Depth 20 -Compress) -cne ($testedAfter | ConvertTo-Json -Depth 20 -Compress)) {
    throw 'Validation executables changed during tests.'
}
foreach ($line in [IO.File]::ReadAllLines($inputsPath)) {
    if ($line -cnotmatch '^(.+):([0-9a-f]{64})$') { throw 'Build inventory changed during tests.' }
    $relative = $Matches[1]; $digest = $Matches[2]
    if ((Get-FileHash -LiteralPath (Get-KqChildPath $projectRoot $relative) -Algorithm SHA256).Hash -ine $digest) { throw "Source changed during tests: $relative" }
}
$report = [ordered]@{
    schema = 1; releaseId = $releaseId; sourceSha256 = $identity.sourceSha256; profileSha256 = $identity.profileSha256
    artifacts = $after; suite = [ordered]@{ total = $total; passed = $total; failed = 0 }
    evidence = @([ordered]@{ kind = 'ctest'; report = 'ctest-results.xml'; sha256 = (Get-KqArtifactRecord $junit).sha256 },
        [ordered]@{ kind = 'test-binaries'; artifacts = $testBinaries })
}
Write-KqAtomicJson (Get-KqChildPath $reportRoot 'test-report.json') $report
Write-Host "Passed $total tests. Bound report: $(Join-Path $reportRoot 'test-report.json')"
Write-Host 'This automated report does not substitute for real-client, visual-DPI or practical performance review.'
