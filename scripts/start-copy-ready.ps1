param([switch]$PrepareOnly, [switch]$SelectClient)
$ErrorActionPreference = 'Stop'
try {
    $bundleRoot = [IO.Path]::GetFullPath($PSScriptRoot)
    $clientRoot = Split-Path -Parent $bundleRoot
    $packageRoot = Join-Path $bundleRoot 'package'
    $toolsRoot = Join-Path $packageRoot 'tools'
    . (Join-Path $toolsRoot 'release-tools.ps1')
    . (Join-Path $toolsRoot 'auto-update-tools.ps1')
    . (Join-Path $toolsRoot 'target-tools.ps1')
    $clientRoot = Assert-KqPlainPath $clientRoot
    $target = $null
    try { $target = Get-KqTarget -OriginalDir $clientRoot -CompatibilityCheck (Join-Path $toolsRoot 'KQPetCompatibilityCheck.exe') } catch { }
    if ($SelectClient -or -not $target -or -not (Test-Path -LiteralPath $target.Path -PathType Leaf)) {
        if ($PrepareOnly) { throw '未找到氪奇主程序，请运行“选择氪奇主程序.cmd”选择 EXE。' }
        if (Test-KqClientRunning $clientRoot) { throw '请先关闭正在运行的氪奇，再更换主程序。' }
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        try {
            $dialog.Title = '选择氪奇主程序 EXE（选择后记住路径）'
            $dialog.Filter = '氪奇主程序 (*.exe)|*.exe'
            $dialog.InitialDirectory = $clientRoot
            $dialog.CheckFileExists = $true
            if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return }
            if ([IO.Path]::GetFileName($dialog.FileName) -like 'KQPet*') { throw '请选择氪奇主程序，不要选择扩展启动器或工具。' }
            $selectionFile = Join-Path $clientRoot 'KQPetClient.txt'
            $selectionTemp = Join-Path $clientRoot ('KQPetClient.' + [guid]::NewGuid().ToString('N') + '.tmp')
            [IO.File]::WriteAllText($selectionTemp,$dialog.FileName,[Text.Encoding]::Unicode)
            if (Test-Path -LiteralPath $selectionFile) { [IO.File]::Replace($selectionTemp,$selectionFile,$null) }
            else { [IO.File]::Move($selectionTemp,$selectionFile) }
        } finally { $dialog.Dispose() }
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
    $ready = $stable -and $selection.ExitCode -eq 0
    $installBundledRelease = -not $ready
    if ($ready -and [string]$selection.Report.releaseId -cne $releaseId) {
        $bundleOrder = Compare-KqReleaseId $releaseId ([string]$selection.Report.releaseId)
        # A copied bundle may update without network access, but an older
        # retained KQPetQuickStart folder must never downgrade an auto-update.
        $installBundledRelease = $null -ne $bundleOrder -and $bundleOrder -gt 0
    }
    if ($installBundledRelease) {
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
    if ($installBundledRelease) {
        $bundledActive = Invoke-KqReleaseCheck $checker @('--resolve', $clientRoot)
        if ($bundledActive.releaseId -cne $releaseId -or $bundledActive.manifestSha256 -ine $package.manifestSha256) {
            throw '当前版本尚未完成更新。请关闭氪奇，再双击“启动精灵工作台”。'
        }
    }
    if (-not $PrepareOnly) {
        # Settings owns the explicit network check. Startup only commits a
        # release that the user already chose and that was fully validated.
        & (Join-Path $toolsRoot 'auto-update.ps1') -ClientRoot $clientRoot -ReleaseCheck $checker -FinalizePendingOnly
    }
    # The updater may have selected a release newer than the retained local
    # bundle. Require a verified active release, not equality with that bundle.
    $active = Invoke-KqReleaseCheck $checker @('--resolve', $clientRoot)
    if (-not $active.releaseId -or -not $active.manifestSha256) {
        throw '当前版本尚未完成更新。请关闭氪奇，再双击“启动精灵工作台”。'
    }
    Invoke-KqReleaseCheck $checker @('--bootstrap-protocol', $launcher) | Out-Null
    if (-not $PrepareOnly) {
        if (Test-KqClientRunning $clientRoot) { throw '氪奇已经启动，请使用现有窗口。' }
        Start-Process -FilePath $launcher -WorkingDirectory $clientRoot -WindowStyle Hidden
    }
} catch {
    if ($PrepareOnly) { throw }
    Write-Host ('启动未完成：' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
