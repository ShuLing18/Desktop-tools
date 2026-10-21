param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\DesktopBox-tests.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\performance'),
    [ValidateRange(1, 5)][int]$Runs = 3
)
$ErrorActionPreference = 'Stop'
$taskExe = (Resolve-Path -LiteralPath $Executable).Path
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskResults = @()
for ($taskRun = 1; $taskRun -le $Runs; $taskRun++) {
    $taskData = Join-Path $taskOutput ('run-' + $taskRun)
    if (Test-Path -LiteralPath $taskData) { throw "Use a fresh output folder: $taskData already exists." }
    New-Item -ItemType Directory -Path $taskData | Out-Null
    New-Item -ItemType File -Path (Join-Path $taskData 'benchmark.flag') | Out-Null
    $taskProcess = Start-Process -FilePath $taskExe -ArgumentList @('--self-test', ('"' + $taskData + '"')) -WindowStyle Hidden -PassThru
    if (-not $taskProcess.WaitForExit(60000)) { throw "Benchmark $taskRun did not finish; inspect its process before retrying." }
    $taskProcess.Refresh()
    if ($taskProcess.ExitCode -ne 0) { throw "Benchmark $taskRun failed." }
    $taskMetrics = Get-Content -LiteralPath (Join-Path $taskData 'metrics.json') -Raw | ConvertFrom-Json
    $taskClosed = @($taskMetrics.samples | Where-Object { $_.stage -eq 'all_panels_closed' })[0]
    $taskResults += [pscustomobject]@{
        run = $taskRun
        closed_private_bytes = $taskClosed.private_bytes
        peak_private_bytes = $taskClosed.peak_private_bytes
        closed_working_set = $taskClosed.working_set
        closed_threads = $taskClosed.threads
        closed_handles = $taskClosed.handles
        closed_gdi_objects = $taskClosed.gdi_objects
        render_60_frames_ms = $taskMetrics.render_60_frames_ms
    }
}
$taskResults | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskOutput 'summary.json') -Encoding utf8
$taskResults | Format-Table -AutoSize
