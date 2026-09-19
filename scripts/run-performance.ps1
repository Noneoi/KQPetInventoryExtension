param(
    [string]$Executable = '',
    [ValidateSet('Priority', 'Full')][string]$Matrix = 'Priority',
    [string[]]$CaseId = @(),
    [string]$OutputDirectory = '',
    [string]$QtRoot = 'D:\Qt\6.6.3\msvc2019_64',
    [ValidateRange(1, 30)][int]$ColdRuns = 1,
    [ValidateRange(0, 100)][int]$Warmup = 1,
    [Alias('Samples')][ValidateRange(1, 1000)][int]$SampleCount = 3,
    [ValidateRange(30, 3600)][int]$RunTimeoutSeconds = 600,
    [switch]$Strict,
    [switch]$SkipCancellation,
    [switch]$NoPhaseProbes
)

$ErrorActionPreference = 'Stop'
# Routine checks favor quick, representative coverage. Detailed sampling and
# historical timing gates are opt-in following the user's budget adjustment.
if ($Strict) {
    if (-not $PSBoundParameters.ContainsKey('Warmup')) { $Warmup = 5 }
    if (-not $PSBoundParameters.ContainsKey('SampleCount')) { $SampleCount = 30 }
}
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $projectRoot 'build-v2\bin\Release\KQAssetAnalysisPerformance.exe' }
$exePath = (Resolve-Path -LiteralPath $Executable).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('build-performance-results\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $outputPath) -and @(Get-ChildItem -LiteralPath $outputPath -Force).Count) {
    throw 'Performance evidence directory is not empty. Select a new output directory.'
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$originalPath = $env:PATH
$utf8 = [Text.UTF8Encoding]::new($false)

function Write-JsonFile([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 30), $utf8)
}
function Source-Snapshot {
    $files = @('tests\performance\asset_analysis_performance.cpp', 'tests\performance\performance_controller_core.h',
               'tests\performance\performance_dataset.cpp', 'tests\performance\performance_dataset.h',
               'scripts\run-performance.ps1',
               'docs\v2.0-reconstruction-plan.md')
    $result = [ordered]@{}
    foreach ($name in $files) {
        $path = Join-Path $projectRoot $name
        if (Test-Path -LiteralPath $path) { $result[$name] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    }
    foreach ($path in @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -File -Recurse |
        Where-Object { $_.Extension -in @('.cpp','.h') } | Sort-Object FullName)) {
        $name = $path.FullName.Substring($projectRoot.Length + 1)
        $result[$name] = (Get-FileHash -LiteralPath $path.FullName -Algorithm SHA256).Hash
    }
    return $result
}
function Quantile($Rows, [string]$Field, [double]$Fraction) {
    $values = @($Rows | ForEach-Object { if ($null -ne $_.$Field) { [double]$_.$Field } } | Sort-Object)
    if (-not $values.Count) { return $null }
    $index = [Math]::Max(0, [Math]::Min($values.Count - 1, [int][Math]::Ceiling($values.Count * $Fraction) - 1))
    return $values[$index]
}
function Compiler-Processes {
    return @(Get-Process -Name 'cl','link','ninja','MSBuild' -ErrorAction SilentlyContinue |
        Select-Object ProcessName, Id)
}

$initialHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
$sourceBefore = Source-Snapshot
$compilerProcesses = Compiler-Processes
if ($Strict -and $compilerProcesses.Count) { throw 'Strict measurements require an idle machine: compiler processes are active.' }
$cpu = Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors
$memory = Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory
$run = [ordered]@{
    schema = 2
    startedAtUtc = [DateTime]::UtcNow.ToString('o')
    executable = $exePath
    executableSha256 = $initialHash
    executableBytes = (Get-Item -LiteralPath $exePath).Length
    harnessSourceHashes = $sourceBefore
    matrix = $Matrix
    strictRequested = [bool]$Strict
    cpu = $cpu
    memory = $memory
    operatingSystem = [Environment]::OSVersion.VersionString
    qtRoot = $QtRoot
    compilerProcessesAtStart = $compilerProcesses
    budgetPolicy = $(if ($Strict) { 'strict-opt-in' } else { 'advisory' })
    warmupPerWarmProcess = $Warmup
    measuredSamplesPerWarmProcess = $SampleCount
    independentColdProcessesPerCase = $ColdRuns
    coldDefinition = 'New process and empty production derived RAM/disk; already observed backpack raw is reported. Warehouse originals are read during the click. OS file cache is not cleared. Fixture ingress is excluded.'
    probes = -not [bool]$NoPhaseProbes
    cancellationIncluded = -not [bool]$SkipCancellation
    sourceBinaryBinding = 'Executable hashes are exact. Source-to-binary correspondence must also be checked by the release verification build.'
}
Write-JsonFile (Join-Path $outputPath 'run.json') $run
$allRows = [Collections.Generic.List[object]]::new()
$summaryRows = [Collections.Generic.List[object]]::new()
$executionRows = [Collections.Generic.List[object]]::new()

function Invoke-Case([string]$Id, [string]$Mode, [string]$Suffix, [int]$CancelAfter = -1, [string]$CancelPhase = 'worker') {
    if ($Id -notmatch '^p[0-9]+-g(actual|[0-9]+)-(zero|sparse|normal|full)-d(0|50|100)-k(8|32)-c(0|1|3|10)-m(0|1|3|10)$') {
        throw ('Invalid case ID: ' + $Id)
    }
    $name = $Id + '-' + $Suffix
    $stdout = Join-Path $outputPath ($name + '.jsonl')
    $stderr = Join-Path $outputPath ($name + '.stderr.log')
    $arguments = @('--case', $Id, '--cache', $Mode, '--timeout-ms', [string]($RunTimeoutSeconds * 1000))
    if ($Mode -eq 'warm') { $arguments += @('--warmup',[string]$Warmup,'--samples',[string]$SampleCount) }
    else { $arguments += @('--warmup','0','--samples','1') }
    if ($CancelAfter -ge 0) { $arguments += @('--cancel-after-ms', [string]$CancelAfter, '--cancel-phase', $CancelPhase, '--no-save', '--no-phase-probes') }
    elseif ($NoPhaseProbes) { $arguments += '--no-phase-probes' }
    $runBinaryHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    $start = [DateTime]::UtcNow
    $process = Start-Process -FilePath $exePath -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Only this newly created, isolated benchmark process may be stopped. A
    # timeout is persisted as failure; it never becomes a missing green row.
    $observedCompilers = [Collections.Generic.List[object]]::new()
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (-not $process.WaitForExit(1000) -and $watch.Elapsed.TotalSeconds -lt ($RunTimeoutSeconds + 15)) {
        foreach ($compiler in Compiler-Processes) { $observedCompilers.Add($compiler) }
    }
    $timedOut = -not $process.HasExited
    if ($timedOut) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; $process.WaitForExit() }
    $process.Refresh()
    $runBinaryHashAfter = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    $execution = [ordered]@{ caseId=$Id; mode=$Mode; suffix=$Suffix; processId=$process.Id;
        startedAtUtc=$start.ToString('o'); endedAtUtc=[DateTime]::UtcNow.ToString('o');
        exitCode=$process.ExitCode; timedOut=$timedOut; executableSha256=$runBinaryHash;
        executableUnchangedDuringRun=($runBinaryHash -eq $runBinaryHashAfter); compilerProcessesObserved=$observedCompilers;
        stdout=[IO.Path]::GetFileName($stdout); stderr=[IO.Path]::GetFileName($stderr) }
    $executionRows.Add([pscustomobject]$execution)
    $rows = [Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $stdout -Encoding UTF8) {
        if (-not $line.Trim()) { continue }
        try { $value = $line | ConvertFrom-Json }
        catch {
            $value = [pscustomobject]@{ kind='invalid-jsonl'; caseId=$Id; error=$_.Exception.Message }
        }
        $value | Add-Member -NotePropertyName executionSuffix -NotePropertyValue $Suffix -Force
        $rows.Add($value); $allRows.Add($value)
    }
    $samples = @($rows | Where-Object { $_.kind -eq 'sample' -and -not $_.warmup })
    $expectedSamples = if ($Mode -eq 'warm') { $SampleCount } else { 1 }
    $expectedWarmups = if ($Mode -eq 'warm') { $Warmup } else { 0 }
    $actualWarmups = @($rows | Where-Object { $_.kind -eq 'sample' -and $_.warmup }).Count
    $errors = [Collections.Generic.List[string]]::new()
    if ($timedOut) { $errors.Add('process_timeout') }
    if ($runBinaryHash -ne $initialHash -or $runBinaryHash -ne $runBinaryHashAfter) { $errors.Add('executable_changed_during_matrix') }
    if ($process.ExitCode -ne 0) { $errors.Add('process_exit_' + $process.ExitCode) }
    if ($samples.Count -ne $expectedSamples) { $errors.Add('sample_count_' + $samples.Count + '_expected_' + $expectedSamples) }
    if ($actualWarmups -ne $expectedWarmups) { $errors.Add('warmup_count_' + $actualWarmups + '_expected_' + $expectedWarmups) }
    if (@($rows | Where-Object { $_.kind -eq 'fatal' -or $_.kind -eq 'timeout' -or $_.kind -eq 'invalid-jsonl' }).Count) { $errors.Add('fatal_or_unparseable_output') }
    if (@($samples | Where-Object { -not $_.correct }).Count) { $errors.Add('calculation_or_complexity_failure') }
    if (@($rows | Where-Object { $_.kind -eq 'dataset' -and
        ($_.derivedRamEntriesBeforeFirstClick -ne 0 -or $_.derivationsBeforeFirstClick -ne 0 -or -not $_.coldDerivedDiskInitiallyEmpty) }).Count) {
        $errors.Add('initial_derived_cache_was_not_empty')
    }
    if (@($samples | Where-Object { $null -ne $_.snapshotSaved -and -not $_.snapshotSaved }).Count) { $errors.Add('snapshot_save_failure') }
    $summary = [ordered]@{ caseId=$Id; mode=$Mode; suffix=$Suffix; measuredSamples=$samples.Count;
        outcomeCounts=@($samples | Group-Object outcome | ForEach-Object { [ordered]@{outcome=$_.Name;count=$_.Count} });
        failures=$errors; strictBudgetsEvaluated=[bool]$Strict }
    foreach ($field in @('clickToVisibleNs','guiToCoreQueueNs','workerQueueMeterPrepareCalculateNs','preparationToFrozenInputNs','sampleThroughPersistenceNs',
        'sortNs','guiModelCommitNs','guiPaintNs','guiSearchFilterSortNs','snapshotSaveNs',
        'probeMaterialIndexNs','probeCatalogCompileNs','probeAccountPrepareNs','cacheComputeActiveWallNs','cacheRawDeriveActiveWallNs','cacheIndexDecodeActiveWallNs',
        'coreRawJsonDecodeNs','coreRawApplyNs','rawDiskReadActiveWallNs','rawDiskReadQueueWaitNs','cacheComputations','cacheHits','readBytes','readFiles',
        'activeInputMeterNs','catalogCompileNs','conditionPrepareNs','candidateComputeNs','productionComputeActiveWallNs',
        'coreEventLoopMaximumGapNs','guiEventLoopMaximumGapNs','computePriorityMaximumWaitNs','cancellationLatencyNs','cancellationEndToEndNs',
        'maximumAtomicComputeNs','maximumInputMeterSliceNs','maximumAtomicCatalogPreparationNs',
        'privateBytesSampledDelta','privateBytesIncrementFromBeforeDataset')) {
        $summary[$field] = [ordered]@{p50=(Quantile $samples $field 0.50);p95=(Quantile $samples $field 0.95);maximum=(Quantile $samples $field 1.0)}
    }
    if ($Strict) {
        if ($observedCompilers.Count) { $errors.Add('concurrent_compilation_contaminated_timing') }
        if (@($rows | Where-Object { $_.kind -eq 'dataset' -and ($_.qtVersion -ne '6.6.3' -or -not $_.releaseAssertionsDisabled -or $_.guiDpiPercent -ne 100) }).Count) {
            $errors.Add('nonbaseline_qt_or_nonrelease_binary')
        }
        if ($Id -eq 'p2000-g200-normal-d100-k8-c3-m3') {
            $budget = if ($Mode -eq 'warm') { 1000000000.0 } else { 2000000000.0 }
            $value = Quantile $samples 'clickToVisibleNs' 0.95
            if ($null -eq $value -or $value -gt $budget) { $errors.Add('click_to_visible_budget') }
        }
        if ($Id -eq 'p10000-g1000-sparse-d100-k8-c3-m3') {
            $value = Quantile $samples 'productionComputeActiveWallNs' 0.95
            if ($null -eq $value -or $value -gt 5000000000.0) { $errors.Add('sparse_compute_budget') }
        }
        $petCount = [int]([regex]::Match($Id,'^p([0-9]+)').Groups[1].Value)
        $searchBudget = if ($petCount -le 2000) { 200000000.0 } else { 500000000.0 }
        $search = Quantile $samples 'guiSearchFilterSortNs' 0.95
        if ($null -ne $search -and $search -gt $searchBudget) { $errors.Add('search_filter_sort_budget') }
        if (@($samples | Where-Object { $_.guiEventLoopMaximumGapNs -gt 50000000 }).Count) { $errors.Add('gui_continuous_block_budget') }
        if (@($samples | Where-Object { $_.logicalMemory.peakChargedBytesLifetime -gt $_.productionLimits.totalBytes }).Count) { $errors.Add('logical_memory_budget') }
        if ($CancelAfter -ge 0 -and @($samples | Where-Object { -not $_.cancellationRequested -or $_.outcome -ne 'Cancelled' -or $_.finishStage -ne $CancelPhase -or $_.cancellationEndToEndNs -gt 100000000 }).Count) {
            $errors.Add('cancellation_not_exercised_or_over_budget')
        }
    }
    $summaryRows.Add([pscustomobject]$summary)
    Write-JsonFile (Join-Path $outputPath 'summary.json') $summaryRows
    Write-JsonFile (Join-Path $outputPath 'executions.json') $executionRows
    Write-Output ($name + ' samples=' + $samples.Count + ' failures=' + $errors.Count)
}

try {
    $env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
    $catalogLines = & $exePath --list-cases
    if ($LASTEXITCODE -ne 0) { throw 'Failed to enumerate performance cases.' }
    $catalog = @($catalogLines | ForEach-Object { $_ | ConvertFrom-Json })
    [IO.File]::WriteAllLines((Join-Path $outputPath 'matrix.jsonl'), [string[]]$catalogLines, $utf8)
    if ($CaseId.Count) { $selected = @($CaseId) }
    elseif ($Matrix -eq 'Full') { $selected = @($catalog | ForEach-Object { $_.caseId }) }
    else { $selected = @('p2000-g200-normal-d100-k8-c3-m3','p10000-g1000-sparse-d100-k8-c3-m3') }
    foreach ($id in $selected) {
        if ($id -notin @($catalog | ForEach-Object { $_.caseId })) { throw ('Case is absent from the declared matrix: ' + $id) }
        Invoke-Case $id 'warm' 'warm'
        for ($cold = 1; $cold -le $ColdRuns; $cold++) { Invoke-Case $id 'cold' ('cold-' + $cold) }
    }
    if (-not $SkipCancellation) {
        Invoke-Case 'p10000-g1000-full-d100-k8-c3-m3' 'cold' 'cancel-25ms' 25
        Invoke-Case 'p10000-g1000-sparse-d100-k8-c3-m3' 'cold' 'cancel-preparation-25ms' 25 'preparation'
    }
    $finalHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    $sourceAfter = Source-Snapshot
    $stable = $initialHash -eq $finalHash -and (($sourceBefore | ConvertTo-Json -Compress) -eq ($sourceAfter | ConvertTo-Json -Compress))
    $run.endedAtUtc = [DateTime]::UtcNow.ToString('o')
    $run.executableUnchanged = $initialHash -eq $finalHash
    $run.harnessSourcesUnchanged = (($sourceBefore | ConvertTo-Json -Compress) -eq ($sourceAfter | ConvertTo-Json -Compress))
    $run.compilerProcessesAtEnd = Compiler-Processes
    $run.selectedCases = $selected
    $run.caseCountDeclared = $catalog.Count
    $run.completed = $true
    # Source edits elsewhere in an active development session do not change the
    # already running executable. Keep the audit flag, but only Strict requires
    # the source tree to remain frozen across the whole sampling run.
    $run.allRequestedRunsPassed = ($initialHash -eq $finalHash) -and
        (-not $Strict -or $stable) -and @($summaryRows | Where-Object { $_.failures.Count -gt 0 }).Count -eq 0
    if ($Strict -and $run.compilerProcessesAtEnd.Count) { $run.allRequestedRunsPassed = $false }
    Write-JsonFile (Join-Path $outputPath 'run.json') $run
    $combined = @($allRows | ForEach-Object { $_ | ConvertTo-Json -Depth 30 -Compress })
    [IO.File]::WriteAllLines((Join-Path $outputPath 'all-results.jsonl'),[string[]]$combined,$utf8)
    Write-Output ('Performance evidence: ' + $outputPath)
    if (-not $run.allRequestedRunsPassed) { exit 2 }
} finally {
    $env:PATH = $originalPath
}
