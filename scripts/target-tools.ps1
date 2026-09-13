# Shared by verification, deployment and removal. These helpers never load or
# execute the original binaries. Target facts come from the runtime registry.
# Native runners such as CTest can pass PowerShell 7's PSModulePath into Windows
# PowerShell 5.1. Load this host's own Utility module explicitly so Get-FileHash
# works without changing the caller's environment or selecting another runtime.
Import-Module -Name (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -Global -ErrorAction Stop

function Get-KqTarget {
    param([string]$OriginalDir = '', [string]$WorkspaceRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)), [string]$CompatibilityCheck = '', [string]$OriginalExe = '')
    if ($OriginalExe) {
        $file = Get-Item -LiteralPath $OriginalExe
        if ($file.PSIsContainer -or $file.Extension -ine '.exe') { throw 'OriginalExe must name an EXE file.' }
        if ($OriginalDir -and [IO.Path]::GetFullPath($OriginalDir).TrimEnd('\') -ine $file.DirectoryName.TrimEnd('\')) { throw 'OriginalExe must remain in OriginalDir.' }
        return [pscustomobject]@{ Path = $file.FullName; Directory = $file.DirectoryName; Name = $file.Name }
    }
    $directories = if ([string]::IsNullOrWhiteSpace($OriginalDir)) {
        @((Get-Item -LiteralPath $WorkspaceRoot)) + @(Get-ChildItem -LiteralPath $WorkspaceRoot -Directory)
    } else {
        @((Get-Item -LiteralPath $OriginalDir))
    }
    $best = $null
    $recognized = @()
    $recognizedVersioned = @()
    foreach ($directory in $directories) {
        if (-not $directory.PSIsContainer) { throw "Target directory is not a directory: $($directory.FullName)" }
        foreach ($file in @(Get-ChildItem -LiteralPath $directory.FullName -Filter '*.exe' -File)) {
            $identity = $null
            try {
                $identity = Invoke-KqCompatibilityCheck -CompatibilityCheck $CompatibilityCheck -TargetPath $file.FullName -Identify
                if ($identity.supported) { $recognized += $file }
            } catch { }
            # Traditional names remain a preference for original-only fallback; unrelated
            # backups such as KQProV1.1.4-old.exe must not outrank a release.
            if ($file.Name -notmatch '^KQPro(?<prefix>V?)(?<version>[0-9]+(?:\.[0-9]+){1,3})\.exe$') { continue }
            $versionText = $Matches['version']
            $hasVersionPrefix = $Matches['prefix'].Length -ne 0
            $parts = @()
            $validVersion = $true
            foreach ($part in $versionText.Split('.')) {
                [uint32]$parsed = 0
                if (-not [uint32]::TryParse($part, [ref]$parsed)) { $validVersion = $false; break }
                $parts += $parsed
            }
            if (-not $validVersion) { continue }
            while ($parts.Count -lt 4) { $parts += 0 }
            # Fixed-width unsigned components retain the launcher's entire uint32
            # domain, including values System.Version (signed int) cannot hold.
            $versionKey = ($parts | ForEach-Object { $_.ToString('D10') }) -join '.'
            $candidate = [pscustomobject]@{ Path = $file.FullName; Directory = $file.DirectoryName; Name = $file.Name; VersionKey = $versionKey; VersionText = $versionText; HasVersionPrefix = $hasVersionPrefix }
            if ($identity -and $identity.supported) { $recognizedVersioned += $candidate }
            if (-not $best) { $best = $candidate; continue }
            $order = [StringComparer]::Ordinal.Compare($candidate.VersionKey, $best.VersionKey)
            # Same-version names use the exact launcher preference: V prefix,
            # shortest spelling, then ordinal name. Path only breaks cross-folder ties.
            if ($order -eq 0) { $order = [int]$candidate.HasVersionPrefix - [int]$best.HasVersionPrefix }
            if ($order -eq 0) { $order = $best.Name.Length - $candidate.Name.Length }
            if ($order -eq 0) { $order = [StringComparer]::OrdinalIgnoreCase.Compare($best.Name, $candidate.Name) }
            if ($order -eq 0) { $order = [StringComparer]::Ordinal.Compare($best.Name, $candidate.Name) }
            if ($order -eq 0) { $order = [StringComparer]::OrdinalIgnoreCase.Compare($best.Path, $candidate.Path) }
            if ($order -eq 0) { $order = [StringComparer]::Ordinal.Compare($best.Path, $candidate.Path) }
            if ($order -gt 0) { $best = $candidate }
        }
    }
    if ($recognized.Count) {
        if ($recognizedVersioned.Count) {
            return @($recognizedVersioned | Sort-Object -Property @{Expression='VersionKey';Descending=$true},
                @{Expression='HasVersionPrefix';Descending=$true}, @{Expression={$_.Name.Length};Ascending=$true}, Name, Path)[0]
        }
        $file = @($recognized | Sort-Object -Property Name, FullName)[0]
        return [pscustomobject]@{ Path = $file.FullName; Directory = $file.DirectoryName; Name = $file.Name }
    }
    if (-not $best) { throw "No recognizable client executable was found in: $($directories.FullName -join ', ')" }
    return $best
}

function Get-KqCompatibilityCheck {
    param([string]$CompatibilityCheck = '')
    if (-not [string]::IsNullOrWhiteSpace($CompatibilityCheck)) {
        if (-not (Test-Path -LiteralPath $CompatibilityCheck -PathType Leaf)) { throw "Compatibility CLI is missing: $CompatibilityCheck" }
        return [IO.Path]::GetFullPath($CompatibilityCheck)
    }
    if ($env:KQPET_COMPATIBILITY_CHECK) { return Get-KqCompatibilityCheck -CompatibilityCheck $env:KQPET_COMPATIBILITY_CHECK }
    $project = Split-Path -Parent $PSScriptRoot
    foreach ($relative in @('build-v2\bin\Release\KQPetCompatibilityCheck.exe', 'build-v2\KQPetCompatibilityCheck.exe',
                             'build-qt663\bin\Release\KQPetCompatibilityCheck.exe', 'build-qt663\KQPetCompatibilityCheck.exe')) {
        $candidate = Join-Path $project $relative
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw 'Build KQPetCompatibilityCheck first, or pass -CompatibilityCheck with its exact path.'
}

function Invoke-KqCompatibilityCheck {
    param([string]$CompatibilityCheck = '', [string]$TargetPath = '', [switch]$Profiles, [switch]$Identify, [string]$ExtensionPath = '')
    $binary = Get-KqCompatibilityCheck -CompatibilityCheck $CompatibilityCheck
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $binary
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $start.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
    if ($Profiles) { $start.Arguments = '--profiles' }
    else {
        # A Windows filename cannot contain a quote. The argument is always
        # one EXE path; never pass an arbitrary command line through here.
        if ([string]::IsNullOrWhiteSpace($TargetPath) -or $TargetPath.Contains('"') -or $TargetPath.EndsWith('\')) {
            throw 'Invalid target EXE path.'
        }
        $verb = if ($Identify) { '--identify' } else { '--target' }
        $start.Arguments = $verb + ' "' + $TargetPath + '"'
        if ($ExtensionPath) {
            if ($ExtensionPath.Contains('"') -or $ExtensionPath.EndsWith('\')) { throw 'Invalid extension DLL path.' }
            $start.Arguments += ' --extension "' + [IO.Path]::GetFullPath($ExtensionPath) + '"'
        }
    }
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Could not start compatibility CLI.' }
        $output = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        $report = $output | ConvertFrom-Json -ErrorAction Stop
        if ($exitCode -ne 0) { throw "Compatibility check rejected target: $($report.error) $stderr" }
        $profileSource = Join-Path (Split-Path -Parent $PSScriptRoot) 'profiles\targets.json'
        if (Test-Path -LiteralPath $profileSource -PathType Leaf) {
            $expected = (Get-FileHash -LiteralPath $profileSource -Algorithm SHA256).Hash
            if ($report.profileDigest -ine $expected) { throw 'Compatibility CLI was built from another profile catalog; rebuild it first.' }
        }
        return $report
    } finally { $process.Dispose() }
}

function Get-KqTargetProfiles {
    param([string]$CompatibilityCheck = '')
    $catalog = Invoke-KqCompatibilityCheck -CompatibilityCheck $CompatibilityCheck -Profiles
    if ($catalog.schema -ne 1 -or -not $catalog.profiles) { throw 'Invalid compiled profile catalog response.' }
    return @($catalog.profiles)
}

function Test-KqTarget {
    param([Parameter(Mandatory = $true)]$Target, [string]$CompatibilityCheck = '', [string]$ExtensionPath = '')
    $report = Invoke-KqCompatibilityCheck -CompatibilityCheck $CompatibilityCheck -TargetPath $Target.Path -ExtensionPath $ExtensionPath
    if (-not $report.supported) { throw "Target compatibility check failed: $($report.error)" }
    return $report
}
