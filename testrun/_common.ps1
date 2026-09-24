# Shared helpers for the RoRo verification scripts (dot-source this file).
#
# Every script used to start a dedicated server, sleep a fixed 45 seconds "for the savegame to
# load" and then send its commands with 4-5 second gaps. A test savegame is actually ready in about
# a second, so instead:
#
#   * the server is polled with a cheap `save` command until the savegame file appears (which proves
#     that the game world and the console exist),
#   * commands are sent with a short delay and the script ends with `quit`, so the process exit
#     itself tells us that every command has been carried out (no more "wait and hope" sleeps).
#
# A whole script therefore takes a handful of seconds instead of ~1.5 minutes, and scripts can also
# be run in parallel (see run_all.ps1).

function Get-RoRoRoot
{
    <#
    .SYNOPSIS
    The repository root (the parent of testrun\).
    #>
    return (Split-Path -Parent $PSScriptRoot)
}

function Get-RoRoTestConfig
{
    <#
    .SYNOPSIS
    The config the verification scripts use; $env:RVTEST_CFG overrides it (for parallel runs).
    #>
    param([string]$Root = (Get-RoRoRoot))
    if ($env:RVTEST_CFG) { return $env:RVTEST_CFG }
    return (Join-Path $Root 'build\roro-test.cfg')
}

function Invoke-RoRoTest
{
    <#
    .SYNOPSIS
    Run a dedicated server, wait until its savegame is loaded, send console commands and return the
    console output.

    .PARAMETER Tag
    Unique name of this run: it is used for the readiness probe savegame, so parallel runs must use
    different tags.

    .PARAMETER Commands
    Console commands to send, usually ending with 'quit' (the process exiting is what tells us that
    all of them ran).

    .PARAMETER RawArguments
    A ready-made command line (used by the scripts which build their own); otherwise it is composed
    from -ExtraArgs/-Savegame.
    #>
    param(
        [Parameter(Mandatory)][string]$Tag,
        [Parameter(Mandatory)][string]$Exe,
        [string]$Config = (Get-RoRoTestConfig),
        [string[]]$ExtraArgs = @(),
        [string]$Savegame = '',
        [string]$RawArguments = '',
        [Parameter(Mandatory)][string[]]$Commands,
        [int]$CarrierParts = 0,
        [string]$LogName = '',
        [double]$DelaySec = 1.0,
        [int]$ReadyTimeoutSec = 90,
        [int]$ExitTimeoutSec = 60,
        [string]$WorkDir = (Get-RoRoRoot),
        [switch]$KeepProbe
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    if ($RawArguments) {
        $psi.Arguments = $RawArguments
    } else {
        # -x: do not write the config back, so that a test run cannot change the shared test config
        $parts = @("-c `"$Config`"", '-x', '-D')
        if ($env:RVTEST_PORT) { $parts[-1] = "-D :$($env:RVTEST_PORT)" }
        if ($Savegame) { $parts += "-g `"$Savegame`"" }
        if ($ExtraArgs.Count -gt 0) { $parts += $ExtraArgs }
        $psi.Arguments = ($parts -join ' ')
    }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $WorkDir
    $p = [System.Diagnostics.Process]::Start($psi)

    # Pause the game as early as possible: the game keeps ticking while the savegame loads, and a
    # vehicle which reaches its station during that time may unload itself before the test starts.
    foreach ($i in 1..3) {
        try { $p.StandardInput.WriteLine('pause'); $p.StandardInput.Flush() } catch {}
        Start-Sleep -Milliseconds 500
    }
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()

    # Readiness: 'save <probe>' writes <config dir>\save\<probe>.sav as soon as the game world exists.
    $cfgDir = Split-Path -Parent $Config
    if (-not $cfgDir) { $cfgDir = $WorkDir }
    $probeName = "_ready_$Tag"
    $probePaths = @(
        (Join-Path $cfgDir ("save\{0}.sav" -f $probeName)),
        (Join-Path $WorkDir ("save\{0}.sav" -f $probeName))
    )
    foreach ($path in $probePaths) { Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue }

    $ready = $false
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastProbeSec = -10.0
    while (-not $ready -and $sw.Elapsed.TotalSeconds -lt $ReadyTimeoutSec) {
        # Re-send the probe only every 1.5s: sending it on every poll queues a save for every poll, and
        # a large savegame takes longer than the poll interval (the queue would then keep saving long
        # after the game is ready, which for a 20 MB map means dozens of useless writes).
        if (($sw.Elapsed.TotalSeconds - $lastProbeSec) -ge 1.5) {
            try { $p.StandardInput.WriteLine("save $probeName"); $p.StandardInput.Flush() } catch { break }
            $lastProbeSec = $sw.Elapsed.TotalSeconds
        }
        Start-Sleep -Milliseconds 200
        foreach ($path in $probePaths) { if (Test-Path -LiteralPath $path) { $ready = $true; break } }
        if ($p.HasExited) { break }
    }
    if (-not $KeepProbe) { foreach ($path in $probePaths) { Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue } }

    if ($ready) {
        # The 'which carrier parts may carry' setting defaults to value 2 (bulk / 'oversized' / 'VEHC'
        # cargo only), which refuses the test savegame's wood wagon. The mechanics scripts want the
        # gate open, so they set it to 0 first; the gate itself is verified by verify_carrier_parts.ps1
        # (which switches all three values itself, so it passes -CarrierParts -1).
        $allCommands = @()
        if ($CarrierParts -ge 0) { $allCommands += "setting vehicle.rv_transport_carrier_parts $CarrierParts" }
        $allCommands += $Commands
        foreach ($c in $allCommands) {
            try { $p.StandardInput.WriteLine($c); $p.StandardInput.Flush() } catch {}
            if ($DelaySec -gt 0) { Start-Sleep -Milliseconds ([int]($DelaySec * 1000)) }
        }
        # The console thread runs a command as soon as it is read, so a short settle time after the
        # last one is enough; 'quit' is a graceful shutdown which takes longer than that, so do not
        # wait for the process to end on its own (the output is complete either way).
        Start-Sleep -Milliseconds 1500
    }
    if (-not $p.HasExited) { try { $p.Kill() } catch {} }
    Start-Sleep -Milliseconds 500

    $txt = ""
    try { $txt += $o.Result } catch {}
    try { $txt += $e.Result } catch {}
    if ($LogName) { [System.IO.File]::WriteAllText((Join-Path $PSScriptRoot $LogName), $txt, (New-Object System.Text.UTF8Encoding($false))) }
    return $txt
}
