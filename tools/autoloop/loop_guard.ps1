#requires -Version 5.1
<#
  Autoloop single-instance + attribution guard (GH #83).

  Dot-source from `run-autoloop.ps1`:

      . (Join-Path $PSScriptRoot 'loop_guard.ps1')

  Why this exists (measured, GH #83): the `Local\RiddickVRAutoloop` mutex in
  `run-autoloop.ps1` only guards the *runner* process. When a runner is killed by
  a harness restart, its `opencode run` child keeps working in the checkout while
  the mutex is free, so the next scheduled wake starts a **second agent in the
  same working tree**: two sessions then build into one `dist\`, rebuild each
  other's shadow install mid-run, and race the same git index (torn builds,
  duplicated/staggered units, borrowed live attribution).

  The guard therefore has two parts:

    1. **Liveness, not just a mutex** -- before starting, scan the live process
       list for an `opencode` run carrying this loop's marker and this checkout
       (`Test-LiveWakeProcess`, pure). A hit means a previous wake is still alive
       and the runner skips instead of overlapping.
    2. **Attribution + run identity** -- a lock file (`loop.lock.json`) records
       the run id, checkout, runner pid and start time; `start run id=...` /
       `end run id=...` lines make a future overlap visible in one line, and the
       `end run` line is emitted from `finally` so every exit path closes the
       record.

  Self-test (no game, no launch, no input):
      powershell -NoProfile -File tools\autoloop\loop_guard.ps1 -SelfTest
#>

param([switch]$SelfTest)

# The marker is the `--title` prefix `run-autoloop.ps1` gives its opencode child.
$script:AutoloopMarker = 'RiddickVR autoloop'

$script:GuardAssertions = 0
$script:GuardFailures = @()


function Get-AutoloopLockPath {
    param([Parameter(Mandatory)][string]$Directory)
    return (Join-Path $Directory 'loop.lock.json')
}


function Read-AutoloopLock {
    <# Parse the lock file; a missing, empty or half-written file reads as $null. #>
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
        if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
        return ($raw | ConvertFrom-Json -ErrorAction Stop)
    }
    catch { return $null }
}


function Write-AutoloopLock {
    <# Atomic write (temp + Move-Item) so a killed runner never leaves a torn
       file that a reader has to guess about. #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][int]$RunId,
        [Parameter(Mandatory)][string]$Checkout,
        [Parameter(Mandatory)][int]$RunnerPid,
        [Parameter(Mandatory)][string]$Started
    )
    $payload = [ordered]@{
        runId     = $RunId
        checkout  = $Checkout
        runnerPid = $RunnerPid
        started   = $Started
    }
    $tmp = "$Path.tmp"
    ($payload | ConvertTo-Json -Compress) | Set-Content -LiteralPath $tmp -Encoding ASCII
    Move-Item -LiteralPath $tmp -Destination $Path -Force
    return $Path
}


function Remove-AutoloopLock {
    param([Parameter(Mandatory)][string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    }
}


function Get-AutoloopRunId {
    <# One plus the number of `start run` records; a higher id left in a stale
       lock still wins, so ids never go backwards. #>
    param(
        [Parameter(Mandatory)][string]$StateLog,
        $ExistingLock = $null
    )
    $n = 0
    if (Test-Path -LiteralPath $StateLog) {
        $n = @(Select-String -LiteralPath $StateLog -Pattern 'start run' `
                -ErrorAction SilentlyContinue).Count
    }
    if ($null -ne $ExistingLock -and $ExistingLock.PSObject.Properties['runId']) {
        $prev = 0
        if ([int]::TryParse([string]$ExistingLock.runId, [ref]$prev)) {
            if ($prev -gt $n) { $n = $prev }
        }
    }
    return ($n + 1)
}


function Test-LiveWakeProcess {
    <# Pure: the live processes that look like a peer wake for this checkout.
       A process counts when its command line carries the autoloop marker *and*
       the checkout path, and it is not this process. #>
    param(
        [object[]]$Processes,
        [string]$Checkout = '',
        [string]$Marker = $script:AutoloopMarker,
        [int]$SelfPid = $PID
    )
    $hits = @()
    foreach ($p in @($Processes)) {
        if ($null -eq $p) { continue }
        $cmd = [string]$p.CommandLine
        if ([string]::IsNullOrWhiteSpace($cmd)) { continue }
        $procId = 0
        if ($p.PSObject.Properties['ProcessId']) { $procId = [int]$p.ProcessId }
        if ($procId -eq $SelfPid) { continue }
        $cmp = [System.StringComparison]::OrdinalIgnoreCase
        if ($cmd.IndexOf($Marker, $cmp) -lt 0) { continue }
        if (-not [string]::IsNullOrEmpty($Checkout) -and
            $cmd.IndexOf($Checkout, $cmp) -lt 0) { continue }
        $hits += $p
    }
    return $hits
}


function Get-AutoloopProcesses {
    <# The live process list (impure). Never fails the caller: a query error
       returns an empty list rather than blocking the loop. #>
    try {
        return @(Get-CimInstance Win32_Process -ErrorAction Stop |
                 Select-Object ProcessId, Name, CommandLine)
    }
    catch { return @() }
}


function Assert-AutoloopGuard {
    param([bool]$Condition, [string]$Message)
    $script:GuardAssertions++
    if (-not $Condition) { $script:GuardFailures += $Message }
}


function Test-AutoloopGuard {
    <# Assertions over the pure decisions + the lock round-trip. No game, no
       launch, no input, no dependency on a live loop. #>
    $script:GuardAssertions = 0
    $script:GuardFailures = @()

    # 1. Liveness decision: exactly the peer (marker + this checkout), not a
    #    different job, not another checkout, not an empty command line, and
    #    never the caller itself.
    $procs = @(
        [pscustomobject]@{ ProcessId = 111; Name = 'node.exe'
                           CommandLine = 'node opencode run --title RiddickVR autoloop 20261018-015901 C:\Users\GigelCastel\riddick-vr' },
        [pscustomobject]@{ ProcessId = 222; Name = 'node.exe'
                           CommandLine = 'node opencode run --title Some other job C:\Users\GigelCastel\riddick-vr' },
        [pscustomobject]@{ ProcessId = 333; Name = 'node.exe'
                           CommandLine = 'node opencode run --title RiddickVR autoloop D:\other\checkout' },
        [pscustomobject]@{ ProcessId = 444; Name = 'opencode.exe'; CommandLine = '' },
        [pscustomobject]@{ ProcessId = $PID; Name = 'powershell.exe'
                           CommandLine = 'powershell -File C:\Users\GigelCastel\riddick-vr\tools\autoloop\run-autoloop.ps1' }
    )
    $hits = @(Test-LiveWakeProcess -Processes $procs -Checkout 'C:\Users\GigelCastel\riddick-vr')
    Assert-AutoloopGuard ($hits.Count -eq 1) "expected one live wake hit, got $($hits.Count)"
    Assert-AutoloopGuard ($hits.Count -ge 1 -and $hits[0].ProcessId -eq 111) 'the hit must be the peer opencode child for this checkout'
    $selfHits = @(Test-LiveWakeProcess -Processes $procs -Checkout 'C:\Users\GigelCastel\riddick-vr' -SelfPid 111)
    Assert-AutoloopGuard ($selfHits.Count -eq 0) 'a process is never its own peer'
    Assert-AutoloopGuard (@(Test-LiveWakeProcess -Processes @() -Checkout 'x').Count -eq 0) 'an empty process list has no live wake'
    Assert-AutoloopGuard (@(Test-LiveWakeProcess -Processes $procs -Checkout '').Count -eq 2) 'without a checkout the marker alone matches every autoloop run'

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("autoloop-guard-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        # 2. Run identity from the shared STATE.log.
        $stateLog = Join-Path $tmp 'STATE.log'
        @('start run', 'end run', 'start run id=2 checkout=x pid=1', 'junk') |
            Set-Content -LiteralPath $stateLog
        Assert-AutoloopGuard ((Get-AutoloopRunId -StateLog $stateLog) -eq 3) 'run id counts start lines and adds one'
        Assert-AutoloopGuard ((Get-AutoloopRunId -StateLog $stateLog -ExistingLock ([pscustomobject]@{ runId = 9 })) -eq 10) 'a higher id in a stale lock wins'
        Assert-AutoloopGuard ((Get-AutoloopRunId -StateLog (Join-Path $tmp 'none.log')) -eq 1) 'a missing log still yields id 1'
        $lock = Get-AutoloopLockPath -Directory $tmp
        Assert-AutoloopGuard ($lock -eq (Join-Path $tmp 'loop.lock.json')) 'lock path is loop.lock.json'

        # 3. Lock round-trip + tolerant reads.
        Write-AutoloopLock -Path $lock -RunId 7 -Checkout 'C:\repo' -RunnerPid 4242 -Started '2026-10-18T02:00:00' | Out-Null
        $read = Read-AutoloopLock -Path $lock
        Assert-AutoloopGuard ($read.runId -eq 7 -and $read.runnerPid -eq 4242 -and $read.checkout -eq 'C:\repo') 'the lock round-trips id, pid and checkout'
        Set-Content -LiteralPath $lock -Value '{"runId": 7, "runnerPid"'
        Assert-AutoloopGuard ($null -eq (Read-AutoloopLock -Path $lock)) 'a half-written lock reads as no lock'
        Set-Content -LiteralPath $lock -Value ''
        Assert-AutoloopGuard ($null -eq (Read-AutoloopLock -Path $lock)) 'an empty lock reads as no lock'
        Assert-AutoloopGuard ($null -eq (Read-AutoloopLock -Path (Join-Path $tmp 'absent.json'))) 'a missing lock reads as no lock'
        Remove-AutoloopLock -Path (Join-Path $tmp 'absent.json')
        Assert-AutoloopGuard (-not (Test-Path -LiteralPath (Join-Path $tmp 'absent.json'))) 'removing a missing lock is a no-op'
        Remove-AutoloopLock -Path $lock
        Assert-AutoloopGuard (-not (Test-Path -LiteralPath $lock)) 'removing the lock deletes it'
    }
    finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ($script:GuardFailures.Count -gt 0) {
        foreach ($f in $script:GuardFailures) { Write-Host "  FAIL $f" }
        Write-Host "FAIL loop guard self-test ($($script:GuardAssertions) assertions)"
        return 1
    }
    Write-Host "PASS loop guard self-test ($($script:GuardAssertions) assertions)"
    return 0
}


# Run the self-test when invoked (or asked), but stay silent when dot-sourced.
if ($SelfTest -or $MyInvocation.InvocationName -ne '.') {
    exit (Test-AutoloopGuard)
}
