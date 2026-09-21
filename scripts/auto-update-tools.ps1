# Pure helpers shared by the GitHub updater and its offline smoke tests.
# Keep this file compatible with the Windows PowerShell 5.1 bundled with Windows.

function ConvertTo-KqReleaseOrderKey {
    param([Parameter(Mandatory = $true)][string]$ReleaseId)
    $match = [regex]::Match($ReleaseId,
        '^(?<version>[0-9]+[.][0-9]+[.][0-9]+)-(?<source>[0-9A-Fa-f]{12})-(?<time>[0-9]{8}T[0-9]{6}Z)$',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $match.Success) { return $null }
    try { $version = [version]::Parse($match.Groups['version'].Value) }
    catch { return $null }
    return [pscustomobject]@{
        Version = $version
        Time = $match.Groups['time'].Value
    }
}

function Compare-KqReleaseId {
    param([Parameter(Mandatory = $true)][string]$Left,
          [Parameter(Mandatory = $true)][string]$Right)
    if ($Left -ceq $Right) { return 0 }
    $leftKey = ConvertTo-KqReleaseOrderKey $Left
    $rightKey = ConvertTo-KqReleaseOrderKey $Right
    if ($null -eq $leftKey -or $null -eq $rightKey) { return $null }
    $versionOrder = $leftKey.Version.CompareTo($rightKey.Version)
    if ($versionOrder -ne 0) { return [Math]::Sign($versionOrder) }
    $timeOrder = [string]::CompareOrdinal($leftKey.Time, $rightKey.Time)
    if ($timeOrder -ne 0) { return [Math]::Sign($timeOrder) }
    # Two builds made during the same second have no trustworthy ordering.
    return 0
}

function Assert-KqGitHubAssetUrl {
    param([Parameter(Mandatory = $true)][string]$Url)
    $uri = $null
    if (-not [Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri) -or
        $uri.Scheme -cne 'https' -or $uri.Host -ine 'github.com') {
        throw 'Release asset URL is not an HTTPS github.com download.'
    }
    return $uri.AbsoluteUri
}

function Select-KqGitHubReleaseAssets {
    param([Parameter(Mandatory = $true)]$Release)
    if ($Release.draft -ne $false -or $Release.prerelease -ne $false -or
        [string]::IsNullOrWhiteSpace([string]$Release.tag_name)) {
        throw 'Only a published, non-prerelease GitHub release can be installed.'
    }
    $archives = @($Release.assets | Where-Object {
        $_.state -ceq 'uploaded' -and
        [string]$_.name -cmatch '^KQPetInventory-[A-Za-z0-9][A-Za-z0-9._-]{0,127}-win-x64-copy-ready[.]zip$'
    })
    if ($archives.Count -ne 1) {
        throw 'The latest release must contain exactly one win-x64 copy-ready ZIP.'
    }
    $archive = $archives[0]
    $checksums = @($Release.assets | Where-Object {
        $_.state -ceq 'uploaded' -and [string]$_.name -ceq ([string]$archive.name + '.sha256')
    })
    if ($checksums.Count -ne 1) {
        throw 'The copy-ready ZIP must have one matching .sha256 release asset.'
    }
    $checksum = $checksums[0]
    if ([long]$archive.id -le 0 -or [long]$archive.size -le 0 -or [long]$archive.size -gt 536870912) {
        throw 'The release archive metadata is invalid or exceeds 512 MiB.'
    }
    if ([long]$checksum.id -le 0 -or [long]$checksum.size -le 0 -or [long]$checksum.size -gt 4096) {
        throw 'The checksum asset metadata is invalid or exceeds 4 KiB.'
    }
    $archiveUrl = Assert-KqGitHubAssetUrl ([string]$archive.browser_download_url)
    $checksumUrl = Assert-KqGitHubAssetUrl ([string]$checksum.browser_download_url)
    return [pscustomobject]@{
        TagName = [string]$Release.tag_name
        Archive = $archive
        ArchiveUrl = $archiveUrl
        Checksum = $checksum
        ChecksumUrl = $checksumUrl
    }
}

function Read-KqReleaseChecksum {
    param([Parameter(Mandatory = $true)][string]$Path,
          [Parameter(Mandatory = $true)][string]$ArchiveName)
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($file.PSIsContainer -or $file.Length -le 0 -or $file.Length -gt 4096) {
        throw 'The downloaded checksum file has an invalid size.'
    }
    $lines = @([IO.File]::ReadAllLines($file.FullName) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($lines.Count -ne 1) { throw 'The checksum file must contain exactly one nonempty line.' }
    $pattern = '^([0-9A-Fa-f]{64})\s{2,}' + [regex]::Escape($ArchiveName) + '\s*$'
    $match = [regex]::Match($lines[0], $pattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $match.Success) { throw 'The checksum file does not name the selected archive.' }
    return $match.Groups[1].Value.ToLowerInvariant()
}

function Test-KqUpdateArchive {
    param([Parameter(Mandatory = $true)][string]$Path)
    Add-Type -AssemblyName System.IO.Compression -ErrorAction Stop
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
    $archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Path))
    try {
        if ($archive.Entries.Count -le 0 -or $archive.Entries.Count -gt 4096) {
            throw 'The update archive contains an invalid number of entries.'
        }
        $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        [uint64]$total = 0
        foreach ($entry in $archive.Entries) {
            $name = ([string]$entry.FullName).Replace('\', '/')
            if ([string]::IsNullOrWhiteSpace($name) -or $name.Length -gt 512 -or
                $name.StartsWith('/') -or $name.Contains(':')) {
                throw 'The update archive contains an unsafe path.'
            }
            $parts = @($name.Split('/') | Where-Object { $_ -ne '' })
            if ($parts.Count -eq 0 -or @($parts | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count -ne 0) {
                throw 'The update archive contains a traversal path.'
            }
            foreach ($part in $parts) {
                $stem = $part.Split('.')[0]
                if ($part.EndsWith('.') -or $part.EndsWith(' ') -or
                    $stem -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
                    throw 'The update archive contains a Windows-ambiguous path.'
                }
            }
            if (-not $names.Add($name)) { throw 'The update archive contains duplicate paths.' }
            $unixMode = ([uint32]$entry.ExternalAttributes -shr 16) -band 0xF000
            if ($unixMode -eq 0xA000) { throw 'The update archive contains a symbolic link.' }
            if ([uint64]$entry.Length -gt 536870912) { throw 'An update archive entry exceeds 512 MiB.' }
            $total += [uint64]$entry.Length
            if ($total -gt 1073741824) { throw 'The expanded update archive exceeds 1 GiB.' }
        }
        foreach ($required in @(
            'KQPetQuickStart/start.ps1',
            'KQPetQuickStart/package/package.json',
            'KQPetQuickStart/package/tools/KQPetReleaseCheck.exe',
            'KQPetQuickStart/package/tools/deploy.ps1'
        )) {
            if (-not $names.Contains($required)) { throw "The update archive is missing $required." }
        }
        return $true
    } finally { $archive.Dispose() }
}

function Test-KqRecentUpdateCheck {
    param($State, [Parameter(Mandatory = $true)][string]$ActiveReleaseId,
          [int]$MinimumMinutes = 15)
    if ($null -eq $State -or [string]$State.activeReleaseId -cne $ActiveReleaseId) { return $false }
    $checked = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse([string]$State.checkedAtUtc,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal, [ref]$checked)) { return $false }
    return $checked -gt [DateTimeOffset]::UtcNow.AddMinutes(-$MinimumMinutes)
}
