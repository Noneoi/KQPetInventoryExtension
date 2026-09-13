param(
    [string]$QtRoot = 'D:\Qt\6.6.3\msvc2019_64',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [string[]]$Targets = @(),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = if ($BuildDirectory) { $BuildDirectory } else { Join-Path $projectRoot 'build-v2' }
$cmakeCommand = $null
$generator = 'Ninja'

$cmakeOnPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmakeOnPath) {
    $cmakeCommand = $cmakeOnPath.Source
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $vsInstall = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if ($vsInstall) {
        $bundledCmake = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (-not $cmakeCommand -and (Test-Path -LiteralPath $bundledCmake)) {
            $cmakeCommand = $bundledCmake
        }
        $vsDevCmd = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
        if (Test-Path -LiteralPath $vsDevCmd) {
            $environmentCommand = 'call "' + $vsDevCmd + '" -arch=x64 -host_arch=x64 >nul && set'
            $environmentLines = & cmd.exe /d /s /c $environmentCommand
            $pathImported = $false
            foreach ($line in $environmentLines) {
                if ($line -match '^([^=]+)=(.*)$') {
                    $name = $Matches[1]
                    if ($name -ieq 'Path' -and $pathImported) { continue }
                    [Environment]::SetEnvironmentVariable($name, $Matches[2], 'Process')
                    if ($name -ieq 'Path') { $pathImported = $true }
                }
            }
        }
    }
}

if (-not $cmakeCommand) {
    throw 'CMake was not found. Install CMake or the Visual Studio CMake component.'
}

if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw "Qt 6.6.3 development package was not found: $QtRoot"
}

& $cmakeCommand -S $projectRoot -B $buildRoot -G $generator "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildArguments = @('--build', $buildRoot)
if ($Clean) { $buildArguments += '--clean-first' }
if ($Targets.Count -gt 0) {
    foreach ($targetName in $Targets) {
        if ($targetName -notmatch '^[A-Za-z][A-Za-z0-9_]*$') { throw 'Invalid CMake target name.' }
    }
    $buildArguments += @('--target') + $Targets
} else {
    # The complete CMake target graph includes every registered test. A new test
    # must not be silently excluded by an independently maintained target list.
    $buildArguments += @('--target', 'all')
}
& $cmakeCommand @buildArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Build complete: $(Join-Path $buildRoot "bin\$Configuration")"
