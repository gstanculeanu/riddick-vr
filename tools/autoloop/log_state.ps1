#requires -Version 5.1
<#
  RiddickVR autoloop run-trace helper (GH #88).

  `tools/autoloop/logs/STATE.log` is the loop's append-only run trace. It is now
  TRACKED in git (see tools/autoloop/.gitignore) because an agent that REPLACED it
  instead of appending destroyed the whole history (measured wake 84:
  107195 -> 5262 bytes, with no git copy to restore from).

  Use this helper -- never edit the file directly:

      # append one wake line
      powershell -NoProfile -File tools\autoloop\log_state.ps1 -Message "wake99 #76 ..."

      # or from a pipeline
      "wake99 #76 ..." | powershell -NoProfile -File tools\autoloop\log_state.ps1

      # verify the tracked log never shrank (CI / after a wake)
      powershell -NoProfile -File tools\autoloop\log_state.ps1 -Verify

      # self-test (no repo, no network, no game)
      powershell -NoProfile -File tools\autoloop\log_state.ps1 -SelfTest

  Fail-loud by design: an append is REFUSED when the live file is smaller than the
  recorded size mark (a truncation/overwrite is in progress), so the mistake is
  reported instead of silently destroying the trace.

  NOTE: the body is deliberately flat (no begin/process/end). Under
  `powershell.exe -File` the `process` block does not run for a non-pipeline
  invocation, so a parameter collected there is silently dropped.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromPipeline = $true)]
    [string]$Message,
    [switch]$Verify,
    [switch]$SelfTest
)

$RelPath = 'tools/autoloop/logs/STATE.log'
$StateLog = Join-Path $PSScriptRoot 'logs\STATE.log'
$SizeMark = "$StateLog.sizemark"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path


function Get-FileSize {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return ([int64]-1) }
    return ([int64](Get-Item -LiteralPath $Path).Length)
}


function Read-MarkValue {
    <# The recorded size mark is the file's *content*, not its byte length. #>
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return ([int64]-1) }
    $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue
    if ($null -eq $raw) { return ([int64]-1) }
    $value = [int64]0
    if ([int64]::TryParse($raw.Trim(), [ref]$value)) { return $value }
    return ([int64]-1)
}


function Test-Header {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return 'missing' }
    $first = Get-Content -LiteralPath $Path -TotalCount 1 -ErrorAction SilentlyContinue
    if (-not $first -or $first -notmatch 'RiddickVR Autoloop') { return 'bad-header' }
    return 'ok'
}


function Invoke-Append {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$MarkPath,
        [Parameter(Mandatory)][string]$Text
    )
    $size = Get-FileSize -Path $Path
    $mark = Read-MarkValue -Path $MarkPath
    if ($mark -ge 0 -and $size -ge 0 -and $size -lt $mark) {
        throw ("STATE.log has SHRUNK ({0} bytes < recorded {1}) -- refusing to " +
               "append (GH #88): an overwrite/truncation is in progress.") -f $size, $mark
    }
    Add-Content -LiteralPath $Path -Value ("[{0}] {1}" -f (Get-Date -Format o), $Text) -Encoding UTF8
    $new = Get-FileSize -Path $Path
    Set-Content -LiteralPath $MarkPath -Value $new -Encoding ASCII
    return $new
}


function Invoke-Verify {
    $hdr = Test-Header -Path $StateLog
    if ($hdr -ne 'ok') { Write-Host "FAIL STATE.log header: $hdr (GH #88)"; return 1 }
    $working = Get-FileSize -Path $StateLog
    $committed = [int64]-1
    Push-Location $RepoRoot
    try {
        $out = & git cat-file -s "HEAD:$RelPath" 2>$null
        if ($LASTEXITCODE -eq 0 -and $out) { $committed = [int64]$out }
    }
    finally { Pop-Location }
    if ($committed -ge 0 -and $working -lt $committed) {
        Write-Host ("FAIL STATE.log shrank vs HEAD: working={0} < committed={1} (GH #88)" -f $working, $committed)
        return 1
    }
    Write-Host ("PASS STATE.log append-only (working={0} bytes, committed={1}, header=ok)" -f $working, $committed)
    return 0
}


function Invoke-SelfTest {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("log_state-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $log = Join-Path $tmp 'STATE.log'
    $mark = "$log.sizemark"
    $fails = @()
    try {
        '# RiddickVR Autoloop - STATE.log (append-only run trace)' | Set-Content -LiteralPath $log -Encoding UTF8
        $a = Invoke-Append -Path $log -MarkPath $mark -Text 'alpha'
        $b = Invoke-Append -Path $log -MarkPath $mark -Text 'beta'
        if (-not ($b -gt $a)) { $fails += "an append must grow the file ($a -> $b)" }
        $body = Get-Content -LiteralPath $log -Raw
        if ($body -notmatch 'alpha' -or $body -notmatch 'beta') { $fails += 'both appended lines must survive' }
        if ((Test-Header -Path $log) -ne 'ok') { $fails += 'header check should accept the real header' }

        # The mark must be the log's byte size (regression: reading the mark
        # file's own length instead of its content defeats the shrink guard).
        $markVal = [int64](Get-Content -LiteralPath $mark -Raw).Trim()
        if ($markVal -ne $b) { $fails += "the size mark must record the byte size ($markVal != $b)" }

        # Simulate wake 84: a replace-style overwrite truncates the file. The
        # next append must be REFUSED (loud), not silently shrink the trace.
        Set-Content -LiteralPath $log -Value '# RiddickVR Autoloop' -Encoding UTF8
        $refused = $false
        try { Invoke-Append -Path $log -MarkPath $mark -Text 'gamma' | Out-Null }
        catch { $refused = $true }
        if (-not $refused) { $fails += 'an append after a truncation must be refused (GH #88)' }

        Set-Content -LiteralPath $log -Value 'not a header' -Encoding UTF8
        if ((Test-Header -Path $log) -ne 'bad-header') { $fails += 'header check should reject a wrong first line' }
    }
    finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }

    if ($fails.Count -gt 0) {
        foreach ($f in $fails) { Write-Host "  FAIL $f" }
        Write-Host 'FAIL log_state self-test'
        return 1
    }
    Write-Host 'PASS log_state self-test'
    return 0
}


if ($SelfTest) { exit (Invoke-SelfTest) }
if ($Verify) { exit (Invoke-Verify) }

$messages = @()
if (-not [string]::IsNullOrWhiteSpace($Message)) { $messages += $Message }
foreach ($piped in $input) {
    if (-not [string]::IsNullOrWhiteSpace([string]$piped)) { $messages += [string]$piped }
}

if ($messages.Count -eq 0) {
    Write-Host 'usage: log_state.ps1 -Message "<one wake line>" | -Verify | -SelfTest'
    exit 2
}

$last = 0
foreach ($m in $messages) {
    $last = Invoke-Append -Path $StateLog -MarkPath $SizeMark -Text $m
}
Write-Host ("appended to STATE.log (now {0} bytes): {1}" -f $last, ($messages -join ' / '))
exit 0
