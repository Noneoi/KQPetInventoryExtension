param([switch]$PrepareOnly)
$ErrorActionPreference = 'Stop'
try {
    $bundleRoot = [IO.Path]::GetFullPath($PSScriptRoot)
    $clientRoot = Split-Path -Parent $bundleRoot
    $packageRoot = Join-Path $bundleRoot 'package'
    $toolsRoot = Join-Path $packageRoot 'tools'
    . (Join-Path $toolsRoot 'release-tools.ps1')
    $clientRoot = Assert-KqPlainPath $clientRoot
    if (-not @(Get-ChildItem -LiteralPath $clientRoot -Filter 'KQPro*.exe' -File).Count) {
        throw '未找到氪奇主程序。请将启动文件和 KQPetQuickStart 文件夹一起复制到 KQPro*.exe 所在目录。'
    }
    if (Test-KqClientRunning $clientRoot) {
        throw '氪奇正在运行。请先关闭氪奇，再双击“启动精灵工作台”。'
    }
    $package = [IO.File]::ReadAllText((Join-Path $packageRoot 'package.json')) | ConvertFrom-Json
    $releaseId = Assert-KqReleaseId $package.releaseId
    $checker = Join-Path $toolsRoot 'KQPetReleaseCheck.exe'
    Invoke-KqReleaseCheck $checker @('--validate', (Join-Path $packageRoot 'KQPetRuntime'), $releaseId, $package.manifestSha256) | Out-Null
    $launcher = Join-Path $clientRoot 'KQPetLauncher.exe'
    $selection = Invoke-KqReleaseCheck $checker @('--resolve', $clientRoot) -AllowFailure
    $stable = if (Test-Path -LiteralPath $launcher) {
        (Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', $launcher) -AllowFailure).ExitCode -eq 0
    } else { $false }
    $ready = $stable -and $selection.ExitCode -eq 0 -and
        $selection.Report.releaseId -ceq $releaseId -and $selection.Report.manifestSha256 -ieq $package.manifestSha256
    if (-not $ready) {
        Write-Host '正在准备精灵工作台，完成后会自动启动……'
        $hostProgram = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $arguments = @('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',
            (Join-Path $toolsRoot 'deploy.ps1'),'-OriginalDir',$clientRoot,'-PackageDirectory',$packageRoot,'-ReleaseCheck',$checker)
        if ($package.preview -eq $true) { $arguments += '-AllowPreview' }
        $start = New-Object System.Diagnostics.ProcessStartInfo
        $start.FileName = $hostProgram
        $start.Arguments = ($arguments | ForEach-Object { ConvertTo-KqWindowsArgument $_ }) -join ' '
        $start.WorkingDirectory = $clientRoot
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $start
        try {
            if (-not $process.Start()) { throw '自动安装未能启动。' }
            $output = $process.StandardOutput.ReadToEndAsync()
            $errorOutput = $process.StandardError.ReadToEndAsync()
            $process.WaitForExit()
            $log = $output.Result + [Environment]::NewLine + $errorOutput.Result
            [IO.File]::WriteAllText((Join-Path $bundleRoot 'install.log'),$log,(New-Object Text.UTF8Encoding($true)))
            if ($process.ExitCode -ne 0) { throw '自动安装未完成，详情已保存在 KQPetQuickStart\install.log。' }
        } finally { $process.Dispose() }
    }
    # A deploy can successfully stage a running client without activating it.
    # Start only the exact package selected from the actual client directory.
    $active = Invoke-KqReleaseCheck $checker @('--resolve', $clientRoot)
    if ($active.releaseId -cne $releaseId -or $active.manifestSha256 -ine $package.manifestSha256) {
        throw '当前版本尚未完成更新。请关闭氪奇，再双击“启动精灵工作台”。'
    }
    Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', $launcher) | Out-Null
    if (-not $PrepareOnly) {
        if (Test-KqClientRunning $clientRoot) { throw '氪奇已经启动，请使用现有窗口。' }
        Start-Process -FilePath $launcher -WorkingDirectory $clientRoot
    }
} catch {
    if ($PrepareOnly) { throw }
    Write-Host ('启动未完成：' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
