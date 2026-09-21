$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. (Join-Path $scriptRoot 'release-tools.ps1')
. (Join-Path $scriptRoot 'auto-update-tools.ps1')

function Assert-True([bool]$Value, [string]$Message) {
    if (-not $Value) { throw "FAIL: $Message" }
}
function Capture-Failure([scriptblock]$Action, [string]$Message) {
    $failure = $null
    try { & $Action | Out-Null } catch { $failure = $_.Exception.Message }
    Assert-True ([bool]$failure) $Message
    return $failure
}

$old = '3.0.0-0123456789ab-20260919T010203Z'
$newBuild = '3.0.0-abcdef012345-20260920T010203Z'
$newVersion = '3.1.0-0123456789ab-20260919T010203Z'
Assert-True ((Compare-KqReleaseId $newBuild $old) -eq 1) 'a later same-version build was not newer'
Assert-True ((Compare-KqReleaseId $newVersion $newBuild) -eq 1) 'a higher semantic version was not newer'
Assert-True ((Compare-KqReleaseId $old $old) -eq 0) 'an identical release did not compare equal'
Assert-True ($null -eq (Compare-KqReleaseId 'legacy' $old)) 'an unorderable release was assigned an order'

$archiveName = "KQPetInventory-$newVersion-win-x64-copy-ready.zip"
$release = [pscustomobject]@{
    tag_name = 'v3.1.0'; draft = $false; prerelease = $false
    assets = @(
        [pscustomobject]@{id=10;name=$archiveName;state='uploaded';size=1024;browser_download_url="https://github.com/Noneoi/KQPetInventoryExtension/releases/download/v3.1.0/$archiveName"},
        [pscustomobject]@{id=11;name=($archiveName + '.sha256');state='uploaded';size=100;browser_download_url="https://github.com/Noneoi/KQPetInventoryExtension/releases/download/v3.1.0/$archiveName.sha256"}
    )
}
$selected = Select-KqGitHubReleaseAssets $release
Assert-True ($selected.Archive.name -ceq $archiveName -and $selected.TagName -ceq 'v3.1.0') 'valid release assets were not selected'
$release.prerelease = $true
Capture-Failure { Select-KqGitHubReleaseAssets $release } 'a prerelease was accepted' | Out-Null
$release.prerelease = $false
$release.assets[0].browser_download_url = 'http://github.com/unsafe.zip'
Capture-Failure { Select-KqGitHubReleaseAssets $release } 'a non-HTTPS asset was accepted' | Out-Null
$release.assets[0].browser_download_url = "https://github.com/Noneoi/KQPetInventoryExtension/releases/download/v3.1.0/$archiveName"

$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$temporary = Join-Path $temporaryBase ('kqpet-auto-update-smoke-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    $checksum = Join-Path $temporary 'asset.sha256'
    $digest = 'a' * 64
    [IO.File]::WriteAllText($checksum, "$digest  $archiveName`n", (New-Object Text.UTF8Encoding($false)))
    Assert-True ((Read-KqReleaseChecksum $checksum $archiveName) -ceq $digest) 'valid sidecar checksum was rejected'
    [IO.File]::WriteAllText($checksum, "$digest  another.zip`n", (New-Object Text.UTF8Encoding($false)))
    Capture-Failure { Read-KqReleaseChecksum $checksum $archiveName } 'a checksum naming another archive was accepted' | Out-Null

    Add-Type -AssemblyName System.IO.Compression -ErrorAction Stop
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
    $validZip = Join-Path $temporary 'valid.zip'
    $zip = [IO.Compression.ZipFile]::Open($validZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in @('KQPetQuickStart/start.ps1','KQPetQuickStart/package/package.json',
                'KQPetQuickStart/package/tools/KQPetReleaseCheck.exe','KQPetQuickStart/package/tools/deploy.ps1')) {
            $entry = $zip.CreateEntry($name)
            $writer = New-Object IO.StreamWriter($entry.Open())
            $writer.Write('fixture'); $writer.Dispose()
        }
    } finally { $zip.Dispose() }
    Assert-True (Test-KqUpdateArchive $validZip) 'a valid copy-ready archive layout was rejected'

    $unsafeZip = Join-Path $temporary 'unsafe.zip'
    $zip = [IO.Compression.ZipFile]::Open($unsafeZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $entry = $zip.CreateEntry('../escape.txt')
        $writer = New-Object IO.StreamWriter($entry.Open())
        $writer.Write('fixture'); $writer.Dispose()
    } finally { $zip.Dispose() }
    Capture-Failure { Test-KqUpdateArchive $unsafeZip } 'a traversal archive was accepted' | Out-Null

    $ambiguousZip = Join-Path $temporary 'ambiguous.zip'
    $zip = [IO.Compression.ZipFile]::Open($ambiguousZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $entry = $zip.CreateEntry('KQPetQuickStart/package/CON.txt')
        $writer = New-Object IO.StreamWriter($entry.Open())
        $writer.Write('fixture'); $writer.Dispose()
    } finally { $zip.Dispose() }
    Capture-Failure { Test-KqUpdateArchive $ambiguousZip } 'a Windows device path was accepted' | Out-Null
    Write-Host 'PASS: GitHub asset selection, release ordering, checksum binding and ZIP traversal guards'
} finally {
    $resolved = [IO.Path]::GetFullPath($temporary)
    if ([IO.Path]::GetDirectoryName($resolved) -ine $temporaryBase -or
        [IO.Path]::GetFileName($resolved) -notmatch '^kqpet-auto-update-smoke-[0-9a-f]{32}$') {
        throw 'Refusing fixture cleanup outside its checked temporary root.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
