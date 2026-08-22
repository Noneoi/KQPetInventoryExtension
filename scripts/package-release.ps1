param(
    [string]$BuildBin = '',
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildBin)) {
    $BuildBin = Join-Path $projectRoot 'build-qt663\bin\Release'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $projectRoot 'dist'
}

$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(KQPetInventoryExtension\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not read the authoritative project version from CMakeLists.txt.'
}
$version = $Matches[1]
$packageName = "KQPetInventory-v$version-win-x64"
$packageDir = Join-Path $OutputRoot $packageName
$zipPath = Join-Path $OutputRoot "$packageName.zip"
if ((Test-Path -LiteralPath $packageDir) -or (Test-Path -LiteralPath $zipPath)) {
    throw "Release output already exists; move or remove it before packaging: $packageName"
}

$launcher = Join-Path $BuildBin 'KQPetLauncher.exe'
$extension = Join-Path $BuildBin 'KQPetInventory.dll'
foreach ($required in @($launcher, $extension,
                         (Join-Path $projectRoot 'README.md'),
                         (Join-Path $projectRoot 'RELEASE_NOTES.md'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required release input is missing: $required"
    }
}

New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
Copy-Item -LiteralPath $launcher, $extension -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $projectRoot 'RELEASE_NOTES.md') -Destination $packageDir

$buildRoot = Split-Path -Parent (Split-Path -Parent $BuildBin)
$generatedVersion = Join-Path $buildRoot 'generated\version.h'
$buildInfo = @("Version: $version", 'Architecture: x64')
if (Test-Path -LiteralPath $generatedVersion) {
    $generatedText = Get-Content -LiteralPath $generatedVersion -Raw
    foreach ($entry in @(
        @{ Label = 'Commit'; Pattern = '#define KQPET_GIT_COMMIT "([^"]+)"' },
        @{ Label = 'Build time'; Pattern = '#define KQPET_BUILD_TIME_UTC "([^"]+)"' },
        @{ Label = 'Qt'; Pattern = '#define KQPET_QT_VERSION_STRING "([^"]+)"' }
    )) {
        if ($generatedText -match $entry.Pattern) {
            $buildInfo += "$($entry.Label): $($Matches[1])"
        }
    }
}
$buildInfo | Set-Content -LiteralPath (Join-Path $packageDir 'BUILD-INFO.txt') -Encoding utf8

$hashFiles = Get-ChildItem -LiteralPath $packageDir -File | Sort-Object Name
$hashLines = foreach ($file in $hashFiles) {
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($file.Name)"
}
$hashLines | Set-Content -LiteralPath (Join-Path $packageDir 'SHA256SUMS.txt') -Encoding ascii

Compress-Archive -LiteralPath $packageDir -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host "Release package: $zipPath"
