param(
  [Parameter(Mandatory=$true)][string]$DataRoot,
  [switch]$ExtractImage,
  [string]$VisualKey,
  # Comma-separated subset (pets,skills,shop,images,icons,routines); empty checks all.
  [string]$Components = ''
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$resolvedRoot = [IO.Path]::GetFullPath($DataRoot)
$toolsRoot = Join-Path $resolvedRoot 'data-tools'
$pythonRoot = Join-Path $toolsRoot 'python'
$pythonExe = Join-Path $pythonRoot 'python.exe'
$expectedArchive = '4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3'
$markerPath = Join-Path $pythonRoot 'verified.json'

function Send-Progress([string]$Message) {
  @{event='progress'; message=$Message} | ConvertTo-Json -Compress | Write-Output
}
function Test-Python {
  if (!(Test-Path -LiteralPath $pythonExe -PathType Leaf) -or !(Test-Path -LiteralPath $markerPath -PathType Leaf)) { return $false }
  try {
    $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    return $marker.archiveSha256 -eq $expectedArchive -and $marker.entrySha256 -eq (Get-FileHash -LiteralPath $pythonExe -Algorithm SHA256).Hash
  } catch { return $false }
}

$scratchPath = $null
$bootstrapLock = $null
try {
  [IO.Directory]::CreateDirectory($toolsRoot) | Out-Null
  # A second client using the same data root must not replace a runtime while
  # the first client's manual update is still using it.
  $bootstrapLock = New-Object IO.FileStream((Join-Path $toolsRoot 'bootstrap.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
  if (!(Test-Python)) {
    if ($ExtractImage) { throw 'Please run a full data update first to prepare local picture tools.' }
    Send-Progress 'Preparing local data update tools (Python)'
    [IO.Directory]::CreateDirectory($toolsRoot) | Out-Null
    $scratchPath = Join-Path $toolsRoot ('bootstrap-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($scratchPath) | Out-Null
    $archivePath = Join-Path $scratchPath 'python.zip'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $client = New-Object Net.WebClient
    try { $client.DownloadFile('https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip', $archivePath) }
    finally { $client.Dispose() }
    if ((Get-Item -LiteralPath $archivePath).Length -gt 33554432 -or (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $expectedArchive) {
      throw 'Python download checksum mismatch.'
    }
    $stagedRoot = Join-Path $scratchPath 'python'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $stagedRoot)
    $stagedExe = Join-Path $stagedRoot 'python.exe'
    if (!(Test-Path -LiteralPath $stagedExe -PathType Leaf)) { throw 'Python entry missing.' }
    # The script imports its colocated parsers explicitly. The embedded runtime
    # stays isolated from site packages, registry paths, and user PYTHONPATH.
    $markerJson = @{archiveSha256=$expectedArchive;entrySha256=(Get-FileHash -LiteralPath $stagedExe -Algorithm SHA256).Hash} | ConvertTo-Json -Compress
    [IO.File]::WriteAllText((Join-Path $stagedRoot 'verified.json'), $markerJson, (New-Object Text.UTF8Encoding($false)))
    if (Test-Path -LiteralPath $pythonRoot) {
      $checkedRoot = [IO.Path]::GetFullPath($pythonRoot)
      if ($checkedRoot -ne [IO.Path]::GetFullPath((Join-Path $resolvedRoot 'data-tools\python'))) { throw 'Unsafe runtime replacement path.' }
      Remove-Item -LiteralPath $checkedRoot -Recurse -Force
    }
    Move-Item -LiteralPath $stagedRoot -Destination $pythonRoot
  }
  $helperPath = Join-Path $PSScriptRoot 'public_data_updater.py'
  $pythonArgs = @('-B', '-u', $helperPath, '--data-root', $resolvedRoot, '--baseline', (Join-Path $PSScriptRoot 'baseline'))
  if ($ExtractImage) { $pythonArgs += @('--extract-image', '--visual-key', $VisualKey) }
  elseif ($Components) { $pythonArgs += @('--components', $Components) }
  & $pythonExe @pythonArgs
  exit $LASTEXITCODE
} catch {
  @{event='finished';success=$false;error=$_.Exception.Message;components=@{}} | ConvertTo-Json -Compress | Write-Output
  exit 1
} finally {
  if ($bootstrapLock) { $bootstrapLock.Dispose() }
  if ($scratchPath -and (Test-Path -LiteralPath $scratchPath)) {
    $checkedScratch = [IO.Path]::GetFullPath($scratchPath)
    $allowedParent = [IO.Path]::GetFullPath($toolsRoot).TrimEnd('\') + '\'
    if ($checkedScratch.StartsWith($allowedParent, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $checkedScratch).StartsWith('bootstrap-')) {
      Remove-Item -LiteralPath $checkedScratch -Recurse -Force
    }
  }
}
