# Shared release tooling. No binary is loaded as executable code to inspect its identity.
# All mutable deployment state is kept under one checked runtime root.
Import-Module -Name (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -Global -ErrorAction Stop

function Resolve-KqReleaseCheck {
    param([string]$ReleaseCheck = '', [string]$BuildBin = '')
    $candidates = @()
    if ($ReleaseCheck) { $candidates = @($ReleaseCheck) }
    elseif ($BuildBin) { $candidates = @((Join-Path $BuildBin 'KQPetReleaseCheck.exe')) }
    else {
        $candidates = @((Join-Path $PSScriptRoot 'KQPetReleaseCheck.exe'),
            (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-v2\bin\Release\KQPetReleaseCheck.exe'))
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return [IO.Path]::GetFullPath($candidate) }
    }
    throw 'The shared ReleaseCheck executable is missing. Build it or pass -ReleaseCheck.'
}

function ConvertTo-KqWindowsArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)
    # CommandLineToArgvW escaping, without cmd.exe/PowerShell evaluation.
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Invoke-KqReleaseCheck {
    param([Parameter(Mandatory = $true)][string]$ReleaseCheck,
          [Parameter(Mandatory = $true)][string[]]$Arguments, [switch]$AllowFailure)
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = [IO.Path]::GetFullPath($ReleaseCheck)
    $start.Arguments = ($Arguments | ForEach-Object { ConvertTo-KqWindowsArgument $_ }) -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $start.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Could not start ReleaseCheck.' }
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        $code = $process.ExitCode
        if (-not $stdout.Trim()) { throw "ReleaseCheck did not return a report ($code): $stderr" }
        $result = $stdout | ConvertFrom-Json -ErrorAction Stop
        if ($code -ne 0 -and -not $AllowFailure) { throw "ReleaseCheck rejected the operation ($code): $stdout $stderr" }
        if ($AllowFailure) { return [pscustomobject]@{ ExitCode = $code; Report = $result } }
        return $result
    } finally { $process.Dispose() }
}

function Assert-KqReleaseId {
    param([Parameter(Mandatory = $true)][string]$ReleaseId)
    if ($ReleaseId -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
        $ReleaseId.Contains('..') -or $ReleaseId.EndsWith('.') -or
        $ReleaseId.Split('.')[0] -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') { throw 'Invalid release ID.' }
    return $ReleaseId
}

function Assert-KqPlainPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not [IO.Path]::IsPathRooted($Path)) { throw "An absolute path is required: $Path" }
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.StartsWith('\\') -or $full.Substring(2).Contains(':')) { throw 'Network paths and alternate data streams are not supported for deployment.' }
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $entry = Get-Item -LiteralPath $cursor -Force
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Deployment paths may not contain a junction or symbolic link: $cursor"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $full
}

function Get-KqChildPath {
    param([Parameter(Mandatory = $true)][string]$Root,
          [Parameter(Mandatory = $true)][string]$Relative)
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative.Contains(':')) { throw 'A relative deployment path is required.' }
    $base = (Assert-KqPlainPath $Root).TrimEnd('\')
    $full = [IO.Path]::GetFullPath((Join-Path $base $Relative))
    if (-not $full.StartsWith($base + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Deployment target escapes its root: $Relative"
    }
    return Assert-KqPlainPath $full
}

function Write-KqAtomicJson {
    param([Parameter(Mandatory = $true)][string]$Path,
          [Parameter(Mandatory = $true)]$Value)
    $target = Assert-KqPlainPath $Path
    $temporary = $target + '.tmp-' + [guid]::NewGuid().ToString('N')
    $bytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes(($Value | ConvertTo-Json -Depth 40 -Compress) + "`n")
    $stream = $null
    try {
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
        $stream.Dispose(); $stream = $null
        if (Test-Path -LiteralPath $target) { [IO.File]::Replace($temporary, $target, [System.Management.Automation.Language.NullString]::Value) }
        else { [IO.File]::Move($temporary, $target) }
    } finally {
        if ($stream) { $stream.Dispose() }
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Get-KqArtifactRecord {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Name = '')
    $file = Get-Item -LiteralPath (Assert-KqPlainPath ([IO.Path]::GetFullPath($Path)))
    if ($file.PSIsContainer -or $file.Length -le 0) { throw "A nonempty artifact is required: $Path" }
    $record = [ordered]@{ size = [long]$file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    if ($Name) { $record = [ordered]@{ path = $Name; size = $record.size; sha256 = $record.sha256 } }
    return $record
}

function Open-KqReadPin {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [IO.File]::Open((Assert-KqPlainPath ([IO.Path]::GetFullPath($Path))),
        [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
}

function Get-KqLegacyBaseline {
    param([Parameter(Mandatory = $true)][string]$Launcher,
          [Parameter(Mandatory = $true)][string]$Extension)
    $registryPath = Join-Path $PSScriptRoot 'legacy-baselines.json'
    if (-not (Test-Path -LiteralPath $registryPath -PathType Leaf)) {
        $registryPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'profiles\legacy-baselines.json'
    }
    $registry = [IO.File]::ReadAllText((Assert-KqPlainPath ([IO.Path]::GetFullPath($registryPath)))) | ConvertFrom-Json
    if ($registry.schema -ne 1) { throw 'The frozen legacy baseline registry is missing or unsupported.' }
    $launcherHash = Get-KqArtifactRecord $Launcher
    $extensionHash = Get-KqArtifactRecord $Extension
    $matches = @($registry.baselines | Where-Object {
        $_.launcher.sha256 -ieq $launcherHash.sha256 -and $_.launcher.size -eq $launcherHash.size -and
        $_.extension.sha256 -ieq $extensionHash.sha256 -and $_.extension.size -eq $extensionHash.size
    })
    if ($matches.Count -ne 1) { throw 'The installed launcher/DLL pair does not match a frozen legacy baseline; neither file will be overwritten.' }
    return $matches[0]
}

function Copy-KqVerifiedFile {
    param([Parameter(Mandatory = $true)][string]$Source,
          [Parameter(Mandatory = $true)][string]$Destination, [switch]$Replace)
    $target = Assert-KqPlainPath ([IO.Path]::GetFullPath($Destination))
    $sourceRecord = Get-KqArtifactRecord $Source
    if ((Test-Path -LiteralPath $target) -and -not $Replace) {
        $current = Get-KqArtifactRecord $target
        if ($current.size -ne $sourceRecord.size -or $current.sha256 -cne $sourceRecord.sha256) {
            throw "An immutable destination already contains different bytes: $target"
        }
        return
    }
    $temporary = $target + '.tmp-' + [guid]::NewGuid().ToString('N')
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary
        $copied = Get-KqArtifactRecord $temporary
        if ($sourceRecord.size -ne $copied.size -or $sourceRecord.sha256 -cne $copied.sha256) { throw 'Copied artifact hash differs.' }
        if (Test-Path -LiteralPath $target) { [IO.File]::Replace($temporary, $target, [System.Management.Automation.Language.NullString]::Value) }
        else { [IO.File]::Move($temporary, $target) }
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Enter-KqDeploymentLock {
    param([Parameter(Mandatory = $true)][string]$RuntimeRoot)
    $root = Assert-KqPlainPath $RuntimeRoot
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $lockPath = Get-KqChildPath $root 'deploy.lock'
    try { return [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
    catch { throw 'Another deployment or bootstrap holds the deployment lock. No active release was changed.' }
}

function Test-KqClientRunning {
    param([Parameter(Mandatory = $true)][string]$ClientRoot)
    $root = (Assert-KqPlainPath $ClientRoot).TrimEnd('\')
    $clientNames = @(Get-ChildItem -LiteralPath $root -Filter '*.exe' -File |
        Where-Object { $_.Name -ine 'KQPetLauncher.exe' } | ForEach-Object { $_.BaseName })
    foreach ($process in @(Get-Process -ErrorAction Stop | Where-Object { $clientNames -contains $_.ProcessName })) {
        try { $path = $process.MainModule.FileName }
        catch { throw 'The running client path could not be verified; offline mutation is refused.' }
        if ([string]::IsNullOrWhiteSpace($path)) { throw 'The running client identity is unknown; offline mutation is refused.' }
        if ([IO.Path]::GetDirectoryName($path).Equals($root, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}
