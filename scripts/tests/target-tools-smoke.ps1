$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'target-tools.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "Assertion failed: $Message" }
}
function Assert-Throws {
    param([scriptblock]$Action, [string]$Message)
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    Assert-True $failed $Message
}
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $temporaryRoot ('kqpet-target-smoke-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
try {
    $older = Join-Path $fixtureRoot 'newly-copied-old-client'
    $newer = Join-Path $fixtureRoot 'current-client'
    New-Item -ItemType Directory -Path $older, $newer | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $older 'KQProV1.1.3.exe'), [byte[]]@(0))
    [IO.File]::WriteAllBytes((Join-Path $newer 'KQProV1.1.4.exe'), [byte[]]@(0))
    (Get-Item -LiteralPath $older).LastWriteTime = [datetime]'2099-01-01'
    (Get-Item -LiteralPath $newer).LastWriteTime = [datetime]'2000-01-01'
    Assert-True ((Get-KqTarget -WorkspaceRoot $fixtureRoot).Name -ceq 'KQProV1.1.4.exe') 'version wins over directory modification time'
    [IO.File]::WriteAllBytes((Join-Path $older 'KQPro1.1.10.exe'), [byte[]]@(0))
    Assert-True ((Get-KqTarget -WorkspaceRoot $fixtureRoot).Name -ceq 'KQPro1.1.10.exe') 'numeric ordering and optional V'
    [IO.File]::WriteAllBytes((Join-Path $older 'KQProV1.1.10.exe'), [byte[]]@(0))
    Assert-True ((Get-KqTarget -OriginalDir $older).Name -ceq 'KQProV1.1.10.exe') 'same-version filenames prefer canonical V prefix like the launcher'
    [IO.File]::WriteAllBytes((Join-Path $older 'KQProV01.1.10.exe'), [byte[]]@(0))
    [IO.File]::WriteAllBytes((Join-Path $older 'KQProV1.1.10.0.exe'), [byte[]]@(0))
    Assert-True ((Get-KqTarget -OriginalDir $older).Name -ceq 'KQProV1.1.10.exe') 'same-version filenames prefer shorter spelling like the launcher'
    foreach ($invalidName in @('KQProV99.0-backup.exe', 'KQPro99999999999999.0.exe', 'KQPro.exe', 'KQProV9.1.2.3.4.exe')) {
        [IO.File]::WriteAllBytes((Join-Path $older $invalidName), [byte[]]@(0))
    }
    Assert-True ((Get-KqTarget -WorkspaceRoot $fixtureRoot).Name -ceq 'KQProV1.1.10.exe') 'backup and malformed versions are ignored'
    [IO.File]::WriteAllBytes((Join-Path $older 'KQProV4294967295.0.exe'), [byte[]]@(0))
    Assert-True ((Get-KqTarget -WorkspaceRoot $fixtureRoot).Name -ceq 'KQProV4294967295.0.exe') 'entire uint32 version range is supported like the launcher'
    Assert-True ((Get-KqTarget -OriginalDir $newer).Directory -ceq $newer) 'explicit directory restricts discovery'
    $empty = Join-Path $fixtureRoot 'empty'
    New-Item -ItemType Directory -Path $empty | Out-Null
    Assert-Throws { Get-KqTarget -OriginalDir $empty } 'empty target directory is rejected'

    $profiles = @(Get-KqTargetProfiles)
    Assert-True (@($profiles | Where-Object { $_.version -ceq '1.1.4' }).Count -eq 1) 'V1.1.4 comes from the generated compiled registry'
    Assert-True (@($profiles | Where-Object { $_.version -ceq '1.1.3' }).Count -eq 1) 'V1.1.3 remains a separate compiled profile'
    $v114 = @($profiles | Where-Object { $_.version -ceq '1.1.4' })[0]
    Assert-True ($v114.executableNames -ccontains 'KQProV1.1.4.exe') 'compiled aliases retain the reviewed V1.1.4 name'
    Assert-Throws { Get-KqTargetProfiles -CompatibilityCheck (Join-Path $fixtureRoot 'missing-check.exe') } 'missing shared CLI is rejected without a PowerShell parser fallback'
    $truncatedTarget = Get-KqTarget -OriginalDir $newer
    Assert-Throws { Test-KqTarget -Target $truncatedTarget } 'the shared CLI rejects truncated target PE data'
    $unknownTarget = Get-KqTarget -OriginalDir $older
    Assert-Throws { Test-KqTarget -Target $unknownTarget } 'the shared CLI rejects an unregistered target version'
    # Raw/mapped PE boundaries, exports and mandatory uniqueness now execute
    # once in compatibility_core_smoke, against the actual shared C++ parser.
    # v2 release activation and rollback are covered by release_core_smoke and
    # release_tools_smoke. Target discovery must not retain the old single-DLL
    # pending behavior, which was deliberately removed from deployment.
    . (Join-Path (Split-Path -Parent $PSScriptRoot) 'release-tools.ps1')
    $renamed = Join-Path $empty 'Renamed-client-next.exe'
    [IO.File]::WriteAllBytes($renamed, [byte[]]@(0))
    Assert-True ((Get-KqTarget -OriginalDir $empty -OriginalExe $renamed).Path -ceq $renamed) 'explicit target retains the complete arbitrary EXE path'
    Assert-Throws { Get-KqTarget -OriginalDir $newer -OriginalExe $renamed } 'explicit target cannot silently select a different client directory'
    function Get-Process {
        param($ErrorAction)
        [pscustomobject]@{ ProcessName = 'Renamed-client-next'; MainModule = [pscustomobject]@{ FileName = $renamed } }
    }
    try { Assert-True (Test-KqClientRunning $empty) 'renamed running client must block offline activation' }
    finally { Remove-Item Function:\Get-Process }
    Assert-Throws { Get-KqChildPath $fixtureRoot '..\outside' } 'release paths cannot escape their root'
    Assert-Throws { Assert-KqReleaseId 'CON.json' } 'Windows device release IDs are rejected'
    $immutable = Join-Path $fixtureRoot 'immutable.dll'
    $source = Join-Path $fixtureRoot 'source.dll'
    [IO.File]::WriteAllText($source, 'synthetic revision one')
    Copy-KqVerifiedFile $source $immutable
    $oldHash = (Get-FileHash -LiteralPath $immutable).Hash
    [IO.File]::WriteAllText($source, 'synthetic revision two')
    Assert-Throws { Copy-KqVerifiedFile $source $immutable } 'immutable files cannot be silently replaced'
    $lock = [IO.File]::Open($immutable, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        Assert-Throws { Copy-KqVerifiedFile $source $immutable -Replace } 'failed atomic replacement is visible'
        Assert-True ((Get-FileHash -LiteralPath $immutable).Hash -ceq $oldHash) 'failed replacement keeps the complete old file'
    } finally { $lock.Dispose() }
    Write-Host 'Target script smoke passed: version discovery, compiled registry/CLI rejection and bounded immutable file operations.'
} finally {
    # This deletion is limited to the freshly created synthetic fixture directory.
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $parent = [IO.Path]::GetDirectoryName($resolved).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if ($parent -cne $temporaryRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^kqpet-target-smoke-[0-9a-f]{32}$') {
        throw "Refusing fixture cleanup outside the temporary root: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
