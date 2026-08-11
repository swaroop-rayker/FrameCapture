#Requires -Version 5.1
<#
.SYNOPSIS
    Launches the engine for development, with verbose logging (SPEC.md §21.2).

.DESCRIPTION
    §21.2 specifies this as "launches engine + GUI with verbose logging". **Only the
    engine half exists.** M8 was split (docs/ACCEPTANCE.md): M8a built the transport --
    SPEC.md §15.1's control channel and §3.1's process lifecycle -- and M8b builds the
    PySide6 GUI. Until then this script starts the engine's control plane and prints the
    pipe name, which is what a GUI would connect to and what `fc_gui_host` already does.

    Said plainly rather than left to be discovered: running this today gives you a live
    engine and no window.

    Three modes:

      -Gui        Launch the GUI (SPEC.md §16), which spawns and owns the engine. This
                  is what a user runs.

      (default)   `framecapture-engine --serve` in the foreground, TRACE logging. Ctrl+C
                  stops it through §3.1's console control handler, which is the same path
                  an OS shutdown takes -- so this is also how to exercise it by hand.

      -Probe      The pre-M8a behaviour: print the session preamble, the GPU topology and
                  the encoder selection, then exit. A one-shot look at the rig.

.EXAMPLE
    .\scripts\run-dev.ps1
    .\scripts\run-dev.ps1 -Probe
    .\scripts\run-dev.ps1 -Preset windows-msvc-debug -LogLevel debug
#>
[CmdletBinding()]
param(
    # Which build to run. Must already be built.
    [ValidateSet('windows-msvc-release', 'windows-msvc-relwithdebinfo', 'windows-msvc-debug')]
    [string]$Preset = 'windows-msvc-release',

    # Launch the GUI instead of a bare engine. The GUI spawns its own engine.
    [switch]$Gui,

    # Print diagnostics and exit instead of serving.
    [switch]$Probe,

    [ValidateSet('trace', 'debug', 'info', 'warn', 'error')]
    [string]$LogLevel = 'trace'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $repoRoot "build\$Preset\bin\framecapture-engine.exe"

if (-not (Test-Path $engine)) {
    Write-Error @"
No engine at $engine

Build it first:
  cmake --preset $Preset
  cmake --build --preset $Preset --parallel
"@
}

# SPEC.md §18 puts the logs under %LOCALAPPDATA%; naming the directory here saves the
# first question anyone asks after a run.
$logDir = Join-Path $env:LOCALAPPDATA 'FrameCapture\logs'

Write-Host "engine    : $engine"
Write-Host "log level : $LogLevel"
Write-Host "logs      : $logDir"

$env:FC_LOG_LEVEL = $LogLevel

if ($Gui) {
    $guiPython = Join-Path $env:LOCALAPPDATA 'FrameCapture\tools\guivenv\Scripts\python.exe'
    if (-not (Test-Path $guiPython)) {
        Write-Error "No GUI venv at $guiPython. Run .\scripts\bootstrap.ps1 first."
    }
    Write-Host "mode      : gui`n"
    # The GUI spawns the engine itself (SPEC.md §3.1), so it is told which one rather
    # than being left to search the build tree and possibly pick a different preset.
    $env:FC_ENGINE_EXE = $engine
    & $guiPython -m framecapture_gui --log-level $LogLevel
    exit $LASTEXITCODE
}

if ($Probe) {
    Write-Host "mode      : probe (diagnostics, then exit)`n"
    & $engine
    exit $LASTEXITCODE
}

Write-Host "mode      : serve (SPEC.md §15.1 control channel)"
Write-Host "            a bare engine, no window. Use -Gui for the GUI."
Write-Host "            Ctrl+C stops it via §3.1's console handler.`n"

& $engine --serve
exit $LASTEXITCODE
