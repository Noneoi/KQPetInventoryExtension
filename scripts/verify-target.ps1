param(
    [string]$OriginalDir = '',
    [string]$OriginalExe = '',
    [string]$ExtensionPath = '',
    [string]$CompatibilityCheck = '',
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'target-tools.ps1')
$target = Get-KqTarget -OriginalDir $OriginalDir -OriginalExe $OriginalExe -CompatibilityCheck $CompatibilityCheck
$report = Test-KqTarget -Target $target -CompatibilityCheck $CompatibilityCheck -ExtensionPath $ExtensionPath

Write-Host 'Offline target verification passed using the shared Win32 Compatibility core: bounded x64 PE, unique interface family signatures, Qt ABI, required exports and observed file identity.'
Write-Host "Path: $($report.Path)"
Write-Host "Profile: $($report.Profile); Qt: $($report.QtVersion)"
foreach ($endpoint in $report.Endpoints) {
    Write-Host ('{0}: 0x{1:X} ({2})' -f $endpoint.Name, $endpoint.Rva, $endpoint.Method)
}
Write-Host "SHA-256: $($report.SHA256)"
Write-Host 'The extension also checks compatibility in the running process before hooking. This offline check does not test server login.'
if ($PassThru) { $report }
