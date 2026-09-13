param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [string]$OutputRoot = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')
$source = Assert-KqPlainPath ([IO.Path]::GetFullPath($PackageDirectory))
$package = [IO.File]::ReadAllText((Join-Path $source 'package.json')) | ConvertFrom-Json
if ($package.schema -ne 1 -or $package.bootstrapProtocol -ne 1) { throw 'Unsupported source package.' }
$releaseId = Assert-KqReleaseId $package.releaseId
$checker = Join-Path $source 'tools\KQPetReleaseCheck.exe'
Invoke-KqReleaseCheck $checker @('--validate',(Join-Path $source 'KQPetRuntime'),$releaseId,$package.manifestSha256) | Out-Null
$bootstrap = Get-KqArtifactRecord (Join-Path $source 'KQPetLauncher.exe')
if ($bootstrap.sha256 -cne $package.bootstrap.sha256 -or $bootstrap.size -ne $package.bootstrap.size) { throw 'Source bootstrap digest differs.' }
Invoke-KqReleaseCheck $checker @('--bootstrap-protocol',(Join-Path $source 'KQPetLauncher.exe')) | Out-Null
if (-not $OutputRoot) { $OutputRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist' }
$output = Assert-KqPlainPath ([IO.Path]::GetFullPath($OutputRoot))
$name = 'KQPetInventory-' + $releaseId + '-win-x64-copy-ready'
$root = Get-KqChildPath $output $name
$zip = Get-KqChildPath $output ($name + '.zip')
if ((Test-Path -LiteralPath $root) -or (Test-Path -LiteralPath $zip)) { throw 'Copy-ready package output already exists.' }
$bundle = Get-KqChildPath $root 'KQPetQuickStart'
$payload = Get-KqChildPath $bundle 'package'
[IO.Directory]::CreateDirectory($payload) | Out-Null
$files = @('KQPetLauncher.exe','package.json','README.md','RELEASE_NOTES.md','SHA256SUMS.txt',
    'tools\KQPetReleaseCheck.exe','tools\KQPetCompatibilityCheck.exe','tools\legacy-baselines.json',
    'licenses\MinHook-LICENSE.txt')
foreach ($script in @('release-tools.ps1','target-tools.ps1','verify-target.ps1','deploy.ps1','rollback.ps1','uninstall.ps1')) {
    $files += 'tools\' + $script
}
foreach ($file in @('KQPetLauncher.exe','KQPetInventory.dll','manifest.json','test-report.json')) {
    $files += 'KQPetRuntime\releases\' + $releaseId + '\' + $file
}
foreach ($relative in $files) {
    $target = Get-KqChildPath $payload $relative
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    Copy-KqVerifiedFile (Get-KqChildPath $source $relative) $target
}
# Windows PowerShell 5.1 requires the BOM to recognize Chinese diagnostics.
$script = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'start-copy-ready.ps1'))
[IO.File]::WriteAllText((Join-Path $bundle 'start.ps1'),$script,(New-Object Text.UTF8Encoding($true)))
$command = "@echo off`r`n`"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`" -NoLogo -NoProfile -ExecutionPolicy Bypass -File `"%~dp0KQPetQuickStart\start.ps1`"`r`nif errorlevel 1 pause`r`n"
[IO.File]::WriteAllText((Join-Path $root '启动精灵工作台.cmd'),$command,[Text.Encoding]::ASCII)
$instructions = @'
精灵工作台：复制后双击启动

1. 关闭氪奇。
2. 将本包全部内容复制到氪奇主程序 KQPro*.exe 所在文件夹。
3. 双击“启动精灵工作台.cmd”。首次自动安装，之后自动启动。

请保留 KQPetQuickStart 文件夹，不需要打开里面的程序。
后续更新也按以上三步操作，不需要填写路径或输入命令。
原客户端、现有账号缓存和数据目录配置会保留。
首次使用可在设置中点击“检查数据更新”。
'@
[IO.File]::WriteAllText((Join-Path $root '精灵工作台-使用说明.txt'),$instructions,(New-Object Text.UTF8Encoding($true)))
# Archive the contents, so extraction exposes the entry directly.
Compress-Archive -LiteralPath @((Join-Path $root '启动精灵工作台.cmd'),$bundle,(Join-Path $root '精灵工作台-使用说明.txt')) -DestinationPath $zip -CompressionLevel Optimal
$digest = (Get-KqArtifactRecord $zip).sha256
[IO.File]::WriteAllText(($zip + '.sha256'),($digest + '  ' + [IO.Path]::GetFileName($zip) + [Environment]::NewLine),(New-Object Text.UTF8Encoding($false)))
Write-Host "Copy-ready package: $zip"
