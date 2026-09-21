param(
    [Parameter(Mandatory = $true)][string]$ClientRoot,
    [string]$ReleaseCheck = '',
    [switch]$Force,
    [switch]$FinalizePendingOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')
. (Join-Path $PSScriptRoot 'auto-update-tools.ps1')

$repository = 'Noneoi/KQPetInventoryExtension'
$latestReleaseApi = "https://api.github.com/repos/$repository/releases/latest"
$state = $null
$activeReleaseId = ''
$updateRoot = ''
$stagingRoot = ''

function Write-KqUpdateLog {
    param([Parameter(Mandatory = $true)][string]$Message)
    if (-not $script:updateRoot) { return }
    $line = ('{0} {1}{2}' -f [DateTimeOffset]::Now.ToString('yyyy-MM-dd HH:mm:ss zzz'), $Message, [Environment]::NewLine)
    [IO.File]::AppendAllText((Join-Path $script:updateRoot 'auto-update.log'), $line, (New-Object Text.UTF8Encoding($true)))
}

function Save-KqUpdateState {
    param([Parameter(Mandatory = $true)][string]$Status,
          [long]$AssetId = 0, [string]$TagName = '', [string]$CandidateReleaseId = '',
          [string]$Message = '')
    Write-KqAtomicJson (Join-Path $script:updateRoot 'github-release.json') ([ordered]@{
        schema = 1
        repository = $script:repository
        checkedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
        status = $Status
        assetId = $AssetId
        tagName = $TagName
        candidateReleaseId = $CandidateReleaseId
        activeReleaseId = $script:activeReleaseId
        message = $Message
    })
}

function Get-KqLatestGitHubRelease {
    param([Parameter(Mandatory = $true)][hashtable]$Headers)
    try {
        return Invoke-RestMethod -Method Get -Uri $script:latestReleaseApi -Headers $Headers -TimeoutSec 15
    } catch {
        # GitHub's unauthenticated API is intentionally rate-limited. The
        # public latest-release redirect and expanded-assets fragment expose
        # the same published release without requiring an account token.
        $location = ''
        try {
            Invoke-WebRequest -UseBasicParsing -Method Get -Uri "https://github.com/$script:repository/releases/latest" `
                -Headers @{'User-Agent'=$Headers['User-Agent']} -MaximumRedirection 0 -TimeoutSec 15 | Out-Null
        } catch {
            if ($_.Exception.Response) {
                $response = $_.Exception.Response
                if ($response.Headers.PSObject.Properties.Name -contains 'Location') {
                    $location = [string]$response.Headers.Location
                } elseif ($response.PSObject.Methods.Name -contains 'GetResponseHeader') {
                    $location = [string]$response.GetResponseHeader('Location')
                }
            }
        }
        $releaseUri = $null
        if (-not [Uri]::TryCreate([string]$location,[UriKind]::Absolute,[ref]$releaseUri) -or
            $releaseUri.Scheme -cne 'https' -or $releaseUri.Host -ine 'github.com' -or
            $releaseUri.AbsolutePath -cnotmatch '^/Noneoi/KQPetInventoryExtension/releases/tag/(?<tag>v[0-9]+[.][0-9]+[.][0-9]+)$') {
            throw 'GitHub API 暂时不可用，且无法从官方发行页确认最新版本。'
        }
        $tag = $Matches.tag
        $expandedUrl = "https://github.com/$script:repository/releases/expanded_assets/$tag"
        $expanded = Invoke-WebRequest -UseBasicParsing -Method Get -Uri $expandedUrl `
            -Headers @{'User-Agent'=$Headers['User-Agent']} -TimeoutSec 15
        $prefix = "/$script:repository/releases/download/$tag/"
        $links = @([regex]::Matches([string]$expanded.Content,'href="(?<path>/Noneoi/KQPetInventoryExtension/releases/download/[^"?]+)"') |
            ForEach-Object { [Net.WebUtility]::HtmlDecode($_.Groups['path'].Value) } |
            Where-Object { $_.StartsWith($prefix,[StringComparison]::Ordinal) } | Select-Object -Unique)
        $assets = @()
        $nextId = 1
        foreach ($path in $links) {
            $url = 'https://github.com' + $path
            $uri = [Uri]$url
            $name = [Uri]::UnescapeDataString([IO.Path]::GetFileName($uri.AbsolutePath))
            if ($name -cnotmatch '^KQPetInventory-[A-Za-z0-9][A-Za-z0-9._-]{0,127}-win-x64-copy-ready[.]zip([.]sha256)?$') { continue }
            $head = Invoke-WebRequest -UseBasicParsing -Method Head -Uri $url -MaximumRedirection 5 `
                -Headers @{'User-Agent'=$Headers['User-Agent']} -TimeoutSec 30
            $lengthText = [string]($head.Headers['Content-Length'] | Select-Object -First 1)
            $length = 0L
            if (-not [long]::TryParse($lengthText,[ref]$length) -and
                $head.BaseResponse.PSObject.Properties.Name -contains 'ContentLength') {
                $length = [long]$head.BaseResponse.ContentLength
            }
            if ($length -le 0) { throw "GitHub 发行附件缺少可靠的文件大小：$name" }
            $assets += [pscustomobject]@{id=$nextId;name=$name;state='uploaded';size=$length;browser_download_url=$url}
            $nextId++
        }
        Write-KqUpdateLog 'GitHub API 配额不可用，已通过官方发行页确认版本和附件。'
        return [pscustomobject]@{tag_name=$tag;draft=$false;prerelease=$false;assets=$assets}
    }
}

try {
    $client = Assert-KqPlainPath ([IO.Path]::GetFullPath($ClientRoot))
    $checker = Resolve-KqReleaseCheck -ReleaseCheck $ReleaseCheck -BuildBin $PSScriptRoot
    $dataRoot = Get-KqChildPath $client 'KQPetData'
    $updateRoot = Get-KqChildPath $dataRoot 'updates'
    [IO.Directory]::CreateDirectory($updateRoot) | Out-Null
    $statePath = Join-Path $updateRoot 'github-release.json'
    $selection = Invoke-KqReleaseCheck $checker @('--resolve', $client) -AllowFailure
    if ($selection.ExitCode -ne 0) { throw '当前没有可用于自动更新的已验证版本。' }
    $activeReleaseId = Assert-KqReleaseId ([string]$selection.Report.releaseId)
    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        try {
            $candidateState = [IO.File]::ReadAllText($statePath) | ConvertFrom-Json -ErrorAction Stop
            if ($candidateState.schema -eq 1 -and $candidateState.repository -ceq $repository) { $state = $candidateState }
        } catch { $state = $null }
    }
    # An in-app update cannot replace a DLL loaded by the running client. In
    # that case deploy.ps1 has already copied and validated the immutable
    # release and left an authenticated pending intent. The next normal launch
    # reaches this block before starting the client and commits that intent.
    if ($null -ne $state -and [string]$state.status -ceq 'staged' -and
        -not (Test-KqClientRunning $client)) {
        $pendingPath = Get-KqChildPath $client 'KQPetRuntime\pending.json'
        if (Test-Path -LiteralPath $pendingPath -PathType Leaf) {
            $pending = [IO.File]::ReadAllText($pendingPath) | ConvertFrom-Json -ErrorAction Stop
            $pendingId = Assert-KqReleaseId ([string]$pending.releaseId)
            if ($pending.schema -eq 1 -and $pendingId -ceq [string]$state.candidateReleaseId -and
                [string]$pending.manifestSha256 -match '^[0-9A-Fa-f]{64}$') {
                $activation = Invoke-KqReleaseCheck $checker @('--activate', $client, $pendingId,
                    [string]$pending.manifestSha256)
                if ([string]$activation.releaseId -cne $pendingId -or
                    [string]$activation.manifestSha256 -ine [string]$pending.manifestSha256) {
                    throw '已下载的新版本未能在重启时完成激活。'
                }
                $activeReleaseId = $pendingId
                Save-KqUpdateState 'updated' ([long]$state.assetId) ([string]$state.tagName) $pendingId
                Write-KqUpdateLog "已在重启时激活 $pendingId。"
                $state = [IO.File]::ReadAllText($statePath) | ConvertFrom-Json -ErrorAction Stop
            }
        }
    }
    if ($FinalizePendingOnly) { return }
    if (-not $Force -and (Test-KqRecentUpdateCheck $state $activeReleaseId)) { return }

    # GitHub requires TLS 1.2. Preserve any newer protocols already selected by the host.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $headers = @{
        Accept = 'application/vnd.github+json'
        'X-GitHub-Api-Version' = '2022-11-28'
        'User-Agent' = 'KQPetInventoryExtension-AutoUpdater'
    }
    $release = Get-KqLatestGitHubRelease $headers
    $assets = Select-KqGitHubReleaseAssets $release
    if ($null -ne $state -and [long]$state.assetId -eq [long]$assets.Archive.id -and
        [string]$state.activeReleaseId -ceq $activeReleaseId) {
        Save-KqUpdateState 'unchanged' ([long]$assets.Archive.id) $assets.TagName ([string]$state.candidateReleaseId)
        return
    }

    $stagingBase = Get-KqChildPath $updateRoot 'staging'
    [IO.Directory]::CreateDirectory($stagingBase) | Out-Null
    $stagingRoot = Get-KqChildPath $stagingBase ([guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
    $archivePath = Join-Path $stagingRoot ([string]$assets.Archive.name)
    $checksumPath = Join-Path $stagingRoot ([string]$assets.Checksum.name)
    Invoke-WebRequest -UseBasicParsing -Uri $assets.ChecksumUrl -Headers @{'User-Agent'=$headers['User-Agent']} -TimeoutSec 30 -OutFile $checksumPath
    Invoke-WebRequest -UseBasicParsing -Uri $assets.ArchiveUrl -Headers @{'User-Agent'=$headers['User-Agent']} -TimeoutSec 120 -OutFile $archivePath
    if ((Get-Item -LiteralPath $archivePath).Length -ne [long]$assets.Archive.size -or
        (Get-Item -LiteralPath $checksumPath).Length -ne [long]$assets.Checksum.size) {
        throw '下载后的文件大小与 GitHub Release 元数据不一致。'
    }
    $expectedHash = Read-KqReleaseChecksum $checksumPath ([string]$assets.Archive.name)
    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -cne $expectedHash) { throw '自动更新包的 SHA-256 校验失败。' }
    Test-KqUpdateArchive $archivePath | Out-Null

    $expanded = Join-Path $stagingRoot 'expanded'
    Expand-Archive -LiteralPath $archivePath -DestinationPath $expanded
    $packageRoot = Join-Path $expanded 'KQPetQuickStart\package'
    $packageFile = Join-Path $packageRoot 'package.json'
    $package = [IO.File]::ReadAllText($packageFile) | ConvertFrom-Json -ErrorAction Stop
    if ($package.schema -ne 1 -or $package.preview -ne $false -or $package.bootstrapProtocol -ne 1) {
        throw 'GitHub Release 中的包不是可安装的正式发行包。'
    }
    $candidateReleaseId = Assert-KqReleaseId ([string]$package.releaseId)
    $expectedArchiveName = "KQPetInventory-$candidateReleaseId-win-x64-copy-ready.zip"
    if ([string]$assets.Archive.name -cne $expectedArchiveName) { throw 'Release 附件名与包内 releaseId 不一致。' }
    $manifestPath = Join-Path $packageRoot "KQPetRuntime\releases\$candidateReleaseId\manifest.json"
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json -ErrorAction Stop
    $tagVersion = ([string]$assets.TagName).Trim()
    if ($tagVersion.StartsWith('v', [StringComparison]::OrdinalIgnoreCase)) { $tagVersion = $tagVersion.Substring(1) }
    if ($tagVersion -cne [string]$manifest.identity.version) { throw 'GitHub Release 标签必须是包内版本号（例如 v3.1.0）。' }
    Invoke-KqReleaseCheck $checker @('--validate', (Join-Path $packageRoot 'KQPetRuntime'),
        $candidateReleaseId, [string]$package.manifestSha256) | Out-Null

    $order = Compare-KqReleaseId $candidateReleaseId $activeReleaseId
    if ($candidateReleaseId -ceq $activeReleaseId) {
        Save-KqUpdateState 'current' ([long]$assets.Archive.id) $assets.TagName $candidateReleaseId
        return
    }
    if ($null -eq $order -or $order -le 0) {
        Save-KqUpdateState 'skipped-not-newer' ([long]$assets.Archive.id) $assets.TagName $candidateReleaseId 'Release 不比当前已安装版本新。'
        return
    }

    Write-KqUpdateLog "发现新版本 $candidateReleaseId，正在验证并安装。"
    & (Join-Path $packageRoot 'tools\deploy.ps1') -OriginalDir $client -PackageDirectory $packageRoot -ReleaseCheck $checker
    $activated = Invoke-KqReleaseCheck $checker @('--resolve', $client) -AllowFailure
    if ($activated.ExitCode -eq 0 -and [string]$activated.Report.releaseId -ceq $candidateReleaseId -and
        [string]$activated.Report.manifestSha256 -ieq [string]$package.manifestSha256) {
        $activeReleaseId = $candidateReleaseId
        Save-KqUpdateState 'updated' ([long]$assets.Archive.id) $assets.TagName $candidateReleaseId
        Write-KqUpdateLog "已自动更新并激活 $candidateReleaseId。"
        Write-Host "已自动更新到 $candidateReleaseId。" -ForegroundColor Green
        return
    }
    $pendingPath = Get-KqChildPath $client 'KQPetRuntime\pending.json'
    if ((Test-KqClientRunning $client) -and (Test-Path -LiteralPath $pendingPath -PathType Leaf)) {
        $pending = [IO.File]::ReadAllText($pendingPath) | ConvertFrom-Json -ErrorAction Stop
        if ($pending.schema -eq 1 -and [string]$pending.releaseId -ceq $candidateReleaseId -and
            [string]$pending.manifestSha256 -ieq [string]$package.manifestSha256) {
            Save-KqUpdateState 'staged' ([long]$assets.Archive.id) $assets.TagName $candidateReleaseId
            Write-KqUpdateLog "已下载并验证 $candidateReleaseId；等待客户端关闭后在下次启动时激活。"
            Write-Host "新版 $candidateReleaseId 已准备好；关闭客户端并重新启动即可完成更新。" -ForegroundColor Green
            return
        }
    }
    throw '新版部署结束后既未激活，也没有留下可验证的待激活版本。'
} catch {
    $message = $_.Exception.Message
    try {
        if ($updateRoot) {
            Write-KqUpdateLog ('自动更新未完成：' + $message)
            Save-KqUpdateState 'failed' 0 '' '' $message
        }
    } catch { }
    # Updating is best-effort. A network or release problem must never prevent
    # the already verified local release from starting.
    Write-Warning ('自动更新检查未完成，将继续启动当前版本：' + $message)
} finally {
    if ($stagingRoot) {
        try {
            $resolved = [IO.Path]::GetFullPath($stagingRoot)
            $expectedParent = [IO.Path]::GetFullPath((Join-Path $updateRoot 'staging')).TrimEnd('\')
            if ([IO.Path]::GetDirectoryName($resolved) -ieq $expectedParent -and
                [IO.Path]::GetFileName($resolved) -match '^[0-9a-f]{32}$' -and
                (Test-Path -LiteralPath $resolved)) {
                Remove-Item -LiteralPath $resolved -Recurse -Force
            }
        } catch { }
    }
}
