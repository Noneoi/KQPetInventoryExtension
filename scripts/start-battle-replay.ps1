param(
    [string]$Sample = '',
    [string]$SampleDirectory = 'C:\keqi\config64\brush',
    [string]$QtRoot = 'D:\Qt\6.6.3\msvc2019_64'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$program = Join-Path $projectRoot 'build-v2\bin\Release\KQBattleReplayPreview.exe'
if (-not (Test-Path -LiteralPath $program)) {
    throw 'Build KQBattleReplayPreview first with scripts/build.ps1.'
}
$env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
if ($Sample) { & $program --file $Sample }
elseif (Test-Path -LiteralPath $SampleDirectory -PathType Container) { & $program --folder $SampleDirectory }
else { & $program }
exit $LASTEXITCODE
