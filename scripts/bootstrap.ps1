#Requires -Version 5.1
<#
.SYNOPSIS
    One-time developer setup for FrameCapture.

.DESCRIPTION
    Fetches vcpkg at the exact baseline pinned in vcpkg.json and bootstraps it.
    The baseline commit is the reproducibility guarantee: two clean clones that
    run this script get byte-identical dependency sources.

    Idempotent -- safe to re-run. If VCPKG_ROOT already points at a usable vcpkg
    installation, that one is used and nothing is cloned.
#>
[CmdletBinding()]
param(
    # Clone vcpkg into the repo even if VCPKG_ROOT is set.
    [switch]$ForceLocalVcpkg
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $repoRoot 'vcpkg.json'

if (-not (Test-Path $manifestPath)) {
    throw "vcpkg.json not found at $manifestPath -- run this from a full clone."
}

$baseline = (Get-Content $manifestPath -Raw | ConvertFrom-Json).'builtin-baseline'
if ([string]::IsNullOrWhiteSpace($baseline)) {
    throw 'vcpkg.json has no builtin-baseline. Refusing to bootstrap an unpinned dependency set.'
}
Write-Host "vcpkg baseline: $baseline"

if (-not $ForceLocalVcpkg -and $env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
    Write-Host "Using existing vcpkg at $env:VCPKG_ROOT"
    Write-Warning 'That installation is not pinned to the baseline above. Pass -ForceLocalVcpkg for a reproducible build.'
    exit 0
}

$vcpkgDir = Join-Path $repoRoot 'vcpkg'

if (-not (Test-Path (Join-Path $vcpkgDir '.git'))) {
    Write-Host "Cloning vcpkg into $vcpkgDir ..."
    # blob:none keeps the clone small while retaining every commit, so the
    # baseline checkout below always resolves.
    & git clone --filter=blob:none https://github.com/microsoft/vcpkg.git $vcpkgDir
    if ($LASTEXITCODE -ne 0) { throw 'git clone of vcpkg failed.' }
}

Write-Host "Checking out baseline $baseline ..."
& git -C $vcpkgDir fetch --filter=blob:none origin $baseline
if ($LASTEXITCODE -ne 0) { throw "Could not fetch baseline $baseline." }
& git -C $vcpkgDir checkout --detach $baseline
if ($LASTEXITCODE -ne 0) { throw "Could not check out baseline $baseline." }

$vcpkgExe = Join-Path $vcpkgDir 'vcpkg.exe'
if (-not (Test-Path $vcpkgExe)) {
    Write-Host 'Bootstrapping vcpkg ...'
    & (Join-Path $vcpkgDir 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'bootstrap-vcpkg.bat failed.' }
}

& $vcpkgExe version

# vcpkg downloads over schannel with revocation checking on. On machines that
# cannot reach the CRL/OCSP responder every download dies with
# "curl error 35 (SSL connect error)" and a misleading proxy diagnostic. Detect
# it here, where the fix is one line, rather than 10 minutes into a build.
& curl.exe -sSI -o NUL 'https://github.com' 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host ''
    Write-Warning @'
This machine's certificate revocation check is failing, so vcpkg's downloads
will fail with "curl operation failed with error code 35". Set this in the
shell you build from:

  $env:X_VCPKG_ASSET_SOURCES = 'x-script,curl.exe -L --ssl-no-revoke --fail --create-dirs --output {dst} {url}'

That routes vcpkg's fetches through the system curl with revocation checking
disabled for those requests only. It changes no system or security setting.
'@
}

# ---------------------------------------------------------------------------
# Python venv for the GUI and the Python quality gates (SPEC.md §16, §21.2)
# ---------------------------------------------------------------------------
#
# Outside the repo, next to the lint venv, for the same reason that one is: a venv
# inside the tree ends up in a glob somewhere -- clang-tidy's file list, an installer
# manifest, a `git clean` someone regrets -- and none of that is worth the convenience
# of a shorter path.
#
# Idempotent: an existing venv is reused and `pip install` is a no-op when the
# requirements are already satisfied.

$guiVenv = Join-Path $env:LOCALAPPDATA 'FrameCapture\tools\guivenv'
$guiPython = Join-Path $guiVenv 'Scripts\python.exe'
$requirements = Join-Path $repoRoot 'gui\requirements.txt'

Write-Host ''
Write-Host "GUI venv: $guiVenv"

if (-not (Test-Path $guiPython)) {
    # `py` rather than `python`: on a machine with the Store alias installed, `python`
    # is a stub that opens the Store rather than an interpreter.
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($null -eq $launcher) {
        Write-Warning @'
No Python launcher (`py`) found. SPEC.md §16 requires Python 3.11+ for the GUI.

The engine builds and tests without it -- only `pytest gui/tests` and the ruff/mypy
half of scripts/lint.ps1 need it. Install Python 3.11 or newer and re-run.
'@
    } else {
        Write-Host 'Creating the GUI venv ...'
        & py -m venv $guiVenv
        if ($LASTEXITCODE -ne 0) { throw "python -m venv failed ($LASTEXITCODE)" }
    }
}

if (Test-Path $guiPython) {
    Write-Host 'Installing GUI requirements ...'
    & $guiPython -m pip install --upgrade pip --quiet
    & $guiPython -m pip install -r $requirements --quiet
    if ($LASTEXITCODE -ne 0) { throw "pip install -r $requirements failed ($LASTEXITCODE)" }

    # Editable, so `python -m framecapture_gui` works from any directory and edits to
    # gui/ take effect without reinstalling. Without this the GUI only starts from the
    # repo root with PYTHONPATH set, which is friction that ends up in a README instead
    # of being fixed.
    & $guiPython -m pip install -e $repoRoot --quiet
    if ($LASTEXITCODE -ne 0) { throw "pip install -e $repoRoot failed ($LASTEXITCODE)" }

    $pysideVersion = & $guiPython -c "import PySide6; print(PySide6.__version__)"
    Write-Host "PySide6: $pysideVersion"
}

Write-Host ''
Write-Host 'Done. Next:'
Write-Host '  cmake --preset windows-msvc-release'
Write-Host '  cmake --build --preset windows-msvc-release --parallel'
Write-Host '  ctest --preset windows-msvc-release --output-on-failure'
Write-Host '  pytest gui/tests -v'

# Explicit: without this the script inherits $LASTEXITCODE from the last native
# command above -- including the revocation probe, which exits 35 by design on an
# affected machine. A bootstrap that reports failure after succeeding is worse
# than no bootstrap.
exit 0
