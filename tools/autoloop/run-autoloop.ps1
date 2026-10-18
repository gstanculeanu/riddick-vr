#requires -Version 5.1
<#
  RiddickVR Autoloop runner.
  Launches one unattended OpenCode run with the standing prompt.
  Single-instance via a named mutex and a live-wake scan (GH #83); logs each run
  to logs/, with one attributable run id per wake.
  Registered as scheduled task "RiddickVR-Autoloop" (every 10 min, 24h).
  Disable:  schtasks /Delete /TN "RiddickVR-Autoloop" /F
#>

# Model used by the unattended loop. Change here if a different model is wanted.
$model = 'opencode-go/deepseek-v4.1-flash'

$repo   = 'C:\Users\GigelCastel\riddick-vr'
$dir    = Join-Path $repo 'tools\autoloop'
$logDir = Join-Path $dir 'logs'
$stateLog = Join-Path $logDir 'STATE.log'
$opencode = 'C:\Program Files\nodejs\opencode.cmd'

# GH #83: single-instance + attribution guard. The mutex below only guards this
# *runner*; a wake whose runner is killed leaves its opencode child alive with the
# mutex free, so the guard also scans for a live peer before starting. Self-test:
#   powershell -NoProfile -File tools\autoloop\loop_guard.ps1 -SelfTest
. (Join-Path $dir 'loop_guard.ps1')

# Deep RE: make the extracted Ghidra visible to `rea` inside the loop. (setx is
# user-level; a task launched by the scheduler may not inherit the edited env.)
$env:GHIDRA_INSTALL_DIR = 'C:\Users\GigelCastel\Tools\ghidra_12.1.4_PUBLIC'

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

# Single instance: skip if a run is already in progress.
$mtx = New-Object System.Threading.Mutex($false, 'Local\RiddickVRAutoloop')
if (-not $mtx.WaitOne(0)) {
    "[{0}] skip: previous run still active" -f (Get-Date -Format o) |
        Add-Content -Path $stateLog
    exit 0
}

# GH #83: only ever remove a lock this runner wrote -- a peer's lock is left alone.
$lockOwned = $false

try {
    Set-Location $repo

    # GH #83: liveness, not just the mutex. A previous runner killed by a harness
    # restart releases the mutex while its `opencode run` child keeps working this
    # checkout, so refuse to start a second agent in the same tree.
    $live = @(Test-LiveWakeProcess -Processes (Get-AutoloopProcesses) -Checkout $repo)
    if ($live.Count -gt 0) {
        "[{0}] skip: live wake pid={1} (previous runner gone; mutex free) - GH #83" -f
            (Get-Date -Format o), (($live | ForEach-Object { $_.ProcessId }) -join ',') |
            Add-Content -Path $stateLog
        exit 0
    }

    # Attribution: one run id per wake, recorded in the lock and in STATE.log, so a
    # future overlap is visible from a single line.
    $lockPath = Get-AutoloopLockPath -Directory $dir
    $runId = Get-AutoloopRunId -StateLog $stateLog -ExistingLock (Read-AutoloopLock -Path $lockPath)
    Write-AutoloopLock -Path $lockPath -RunId $runId -Checkout $repo -RunnerPid $PID `
        -Started (Get-Date -Format o) | Out-Null
    $lockOwned = $true

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $outFile = Join-Path $logDir ("run-{0}.log" -f $stamp)
    $errFile = Join-Path $logDir ("run-{0}.err.log" -f $stamp)
    $prompt = Get-Content (Join-Path $dir 'prompt.md') -Raw

    "[{0}] start run id={1} checkout={2} pid={3}" -f
        (Get-Date -Format o), $runId, $repo, $PID | Add-Content -Path $stateLog

    # Native command stderr must not abort the script under Stop semantics.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $ocArgs = @(
            'run', '--auto', '--standalone', '--model', $model,
            '--title', ("RiddickVR autoloop {0}" -f $stamp),
            $prompt
        )
        & $opencode @ocArgs 1> $outFile 2> $errFile
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $prevEap
    }

    "[{0}] end run id={1} exit={2} log={3}" -f (Get-Date -Format o), $runId, $code, (Split-Path $outFile -Leaf) |
        Add-Content -Path $stateLog
}
finally {
    if ($lockOwned) { Remove-AutoloopLock -Path (Get-AutoloopLockPath -Directory $dir) }
    $mtx.ReleaseMutex()
    $mtx.Dispose()
}
