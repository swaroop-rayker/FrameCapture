#Requires -Version 5.1
<#
.SYNOPSIS
    Quality gate: clang-format, clang-tidy, and the banned-pattern grep.

.DESCRIPTION
    Run this before every commit (CLAUDE.md §3). Exits non-zero on the first
    category that fails, after reporting all failures in that category.

    The banned-pattern pass covers the rules clang-tidy cannot express:
    unchecked HRESULT, catch(...) outside a thread entry point, wall-clock APIs
    used for media timing, and timeBeginPeriod (CLAUDE.md §4).

    ruff and mypy are not run yet: gui/ still contains no Python source. M8 was
    split (see docs/ACCEPTANCE.md) -- M8a built the transport, which is C++ and
    covered by the passes above; the GUI itself is M8b, and adding ruff and mypy
    here is its first task, before the first .py file exists rather than after.
#>
[CmdletBinding()]
param(
    # Rewrite files in place instead of failing on a formatting diff.
    [switch]$Fix,
    # Skip clang-tidy (it needs a compile_commands.json; see below).
    [switch]$SkipTidy,
    # Turn every "skipped" into a failure. Pass this in CI.
    #
    # Locally the skips are deliberate: a C++-only contributor with no Python, or a
    # machine without the optional Visual Studio Clang component, should still get the
    # passes that do apply rather than a red run they learn to ignore. In CI the same
    # leniency is a gate that reports clean while checking nothing -- there a tool is
    # missing because the workflow forgot to install it, not because the machine is
    # somebody's laptop. -Strict is the difference between those two situations.
    [switch]$Strict
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$failed = $false

function Write-Skip {
    <#
        A pass that did not run: a warning locally, a failure under -Strict.

        One function rather than a bare warning at each site, so a skip added later
        cannot forget to honour the flag -- which would put the hole straight back.
    #>
    param([Parameter(Mandatory)][string]$Message)

    if ($Strict) {
        Write-Host "SKIPPED (fatal under -Strict): $Message" -ForegroundColor Red
        $script:failed = $true
    }
    else {
        Write-Warning $Message
    }
}

function Find-Tool {
    param([string]$Name)

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * -property installationPath
        if ($vsRoot) {
            $candidate = Join-Path $vsRoot "VC\Tools\Llvm\x64\bin\$Name.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    foreach ($p in @("$env:ProgramFiles\LLVM\bin\$Name.exe", "${env:ProgramFiles(x86)}\LLVM\bin\$Name.exe")) {
        if (Test-Path $p) { return $p }
    }

    # The pip-installed fallback documented in BUILD.md §4.2. The Visual Studio
    # "C++ Clang tools" component is optional and frequently absent; this keeps
    # the lint gate reproducible without requiring an installer round trip.
    $venv = Join-Path $env:LOCALAPPDATA "FrameCapture\tools\lintvenv\Scripts\$Name.exe"
    if (Test-Path $venv) { return $venv }

    return $null
}

$sources = Get-ChildItem -Path (Join-Path $repoRoot 'engine'), (Join-Path $repoRoot 'tests') `
    -Recurse -Include *.cpp, *.h, *.hpp -File -ErrorAction SilentlyContinue

if (-not $sources) {
    Write-Skip 'No C++ sources found under engine/ or tests/.'
}

# --------------------------------------------------------------------------
# clang-format
# --------------------------------------------------------------------------
$clangFormat = Find-Tool 'clang-format'
if (-not $clangFormat) {
    Write-Error @'
clang-format not found. Install one of:
  - Visual Studio Installer -> "C++ Clang tools for Windows"
  - winget install LLVM.LLVM
  - pip install clang-format
'@
    exit 2
}

Write-Host "clang-format: $clangFormat"
if ($sources) {
    if ($Fix) {
        & $clangFormat -i --style=file $sources.FullName
    }
    else {
        & $clangFormat --dry-run --Werror --style=file $sources.FullName
        if ($LASTEXITCODE -ne 0) {
            Write-Host 'clang-format FAILED (run scripts/lint.ps1 -Fix)' -ForegroundColor Red
            $failed = $true
        }
    }
}

# --------------------------------------------------------------------------
# clang-tidy
#
# Needs a compile_commands.json, which the Visual Studio generator cannot
# produce. Point -SkipTidy at CI or configure a Ninja build directory from a
# Developer PowerShell:
#     cmake -B build/tidy -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
#           --preset windows-msvc-debug
# --------------------------------------------------------------------------
if (-not $SkipTidy) {
    $compileDb = Get-ChildItem -Path (Join-Path $repoRoot 'build') -Recurse -Filter compile_commands.json `
        -File -ErrorAction SilentlyContinue | Select-Object -First 1
    $clangTidy = Find-Tool 'clang-tidy'

    if (-not $clangTidy) {
        Write-Skip 'clang-tidy not found; skipping. Install "C++ Clang tools for Windows".'
    }
    elseif (-not $compileDb) {
        Write-Skip 'No compile_commands.json under build/; skipping clang-tidy. See the comment in this script.'
    }
    else {
        Write-Host "clang-tidy: $clangTidy (db: $($compileDb.FullName))"

        # One process per file, in parallel. Serially this takes >10 minutes,
        # because every translation unit re-parses toml++, spdlog and gtest headers;
        # a pre-commit gate that slow is a gate people route around.
        #
        # Only files that appear in the compile database are checked -- clang-tidy
        # cannot analyse a header on its own, and passing one produces a spurious
        # "not found in compilation database" failure.
        $dbPath = $compileDb.DirectoryName
        $dbFiles = (Get-Content $compileDb.FullName -Raw | ConvertFrom-Json).file |
            ForEach-Object { (Resolve-Path -LiteralPath $_ -ErrorAction SilentlyContinue).Path } |
            Where-Object { $_ }
        $targets = $sources | Where-Object { $dbFiles -contains $_.FullName } | Select-Object -Expand FullName

        # A stale database is worse than a missing one: clang-tidy only ever sees
        # the files listed in it, so every .cpp added since the last configure is
        # skipped in silence and the gate reports clean on a shrinking fraction of
        # the tree. This is how 37 findings accumulated behind a green lint run
        # (ENGINEERING_LOG BUG-005), so it is an error, not a warning.
        $missing = $sources |
            Where-Object { $_.Extension -eq '.cpp' -and $dbFiles -notcontains $_.FullName } |
            Select-Object -Expand FullName
        if ($missing) {
            Write-Host 'clang-tidy FAILED: the compile database is stale.' -ForegroundColor Red
            Write-Host "  $($compileDb.FullName)"
            Write-Host "  $($missing.Count) source file(s) are not in it and would have been skipped:"
            $missing | ForEach-Object { Write-Host "    $($_ -replace [regex]::Escape($repoRoot + '\'), '')" }
            Write-Host '  Reconfigure it from a Developer PowerShell:'
            Write-Host '    cmake -B build/tidy -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON --preset windows-msvc-debug'
            $failed = $true
        }

        if (-not $targets) {
            Write-Skip 'No source file matched the compile database; skipping clang-tidy.'
        }
        else {
            $throttle = [Math]::Max(2, [Environment]::ProcessorCount)
            $results = $targets | ForEach-Object -ThrottleLimit $throttle -Parallel {
                $output = & $using:clangTidy -p $using:dbPath --quiet `
                    --extra-arg=-Wno-unused-command-line-argument $_ 2>&1
                if ($LASTEXITCODE -ne 0) {
                    [pscustomobject]@{ File = $_; Output = ($output -join "`n") }
                }
            }

            if ($results) {
                Write-Host 'clang-tidy FAILED' -ForegroundColor Red
                $results | ForEach-Object { Write-Host $_.Output }
                $failed = $true
            }
            else {
                Write-Host "clang-tidy: clean ($($targets.Count) files, $throttle parallel)"
            }
        }
    }
}

# --------------------------------------------------------------------------
# Banned patterns (CLAUDE.md §4)
# --------------------------------------------------------------------------
$banned = @(
    @{ Pattern = 'catch\s*\(\s*\.\.\.\s*\)'; Reason = 'catch(...) is allowed only at a thread entry point; annotate with // FC_THREAD_ENTRY' }
    @{ Pattern = 'catch\s*\([^)]*\)\s*\{\s*\}'; Reason = 'empty catch block' }
    @{ Pattern = '\bGetTickCount(64)?\s*\('; Reason = 'GetTickCount is not a media clock; use QPC (SPEC.md §7.1)' }
    @{ Pattern = 'std::chrono::system_clock'; Reason = 'system_clock is not monotonic; use QPC (SPEC.md §7.1)' }
    @{ Pattern = '\btimeBeginPeriod\s*\('; Reason = 'timeBeginPeriod degrades the whole system' }
    @{ Pattern = '\bnew\s+[A-Za-z_]'; Reason = 'raw new -- use a RAII wrapper' }
    @{ Pattern = '\bdelete\s+[A-Za-z_]'; Reason = 'raw delete -- use a RAII wrapper' }
    @{ Pattern = '(?<!FC_HR\()\bhr\s*=\s*[A-Za-z_][A-Za-z0-9_:]*\s*\('; Reason = 'unchecked HRESULT -- wrap in FC_HR()' }
)

$violations = @()
foreach ($file in $sources) {
    $text = Get-Content -LiteralPath $file.FullName
    for ($i = 0; $i -lt $text.Count; $i++) {
        $line = $text[$i]
        # FC_THREAD_ENTRY is the annotation the catch(...) rule below tells you to
        # use; FC_LINT_OK is the general escape hatch. Both must be honoured, or the
        # rule's own advice does not work.
        #
        # Accepted on the offending line *or* the line above it, matching
        # clang-tidy's NOLINTNEXTLINE convention -- an annotation that needs a
        # justification does not fit on the same line as the code it excuses.
        $annotation = '//\s*(FC_LINT_OK|FC_THREAD_ENTRY)'
        if ($line -match $annotation) { continue }
        if ($i -gt 0 -and $text[$i - 1] -match $annotation) { continue }
        if ($line -match '^\s*(//|\*|/\*)') { continue }

        # Match against code only. String and character literals are data, not
        # code: without this, a test payload of "new content" trips the raw-new
        # rule and a message mentioning "delete the file" trips raw-delete.
        # Trailing line comments are stripped for the same reason.
        $code = $line -replace '"(\\.|[^"\\])*"', '""'
        $code = $code -replace "'(\\.|[^'\\])*'", "''"
        $code = $code -replace '//.*$', ''

        foreach ($rule in $banned) {
            if ($code -match $rule.Pattern) {
                $violations += "{0}:{1}: {2}`n    {3}" -f $file.FullName, ($i + 1), $rule.Reason, $line.Trim()
            }
        }
    }
}

if ($violations) {
    Write-Host 'Banned patterns FAILED:' -ForegroundColor Red
    $violations | ForEach-Object { Write-Host $_ }
    $failed = $true
}
else {
    Write-Host 'Banned patterns: clean'
}

# --------------------------------------------------------------------------
# Overlay capture-exclusion rules (M9.6, SPEC.md §20 row 19)
#
# Two rules, both about one defect: a black rectangle in the recorded video where
# the overlay was.
#
#   * `WDA_MONITOR` (0x01) hides a window from captures by painting **black** into
#     them. It is one hex digit from `WDA_EXCLUDEFROMCAPTURE` (0x11), it is what a
#     pre-2020 search result hands you, and using it is precisely the reported
#     "pill-shaped black cutout" bug. `test_overlay_exclusion.cpp` asserts that it
#     still behaves that way, so the constant is not merely disliked -- it is
#     measured.
#   * `SetWindowDisplayAffinity` is allowed in exactly one module. The whole reason
#     `overlay/exclusion.py` exists is that two call sites is how "the pill is
#     excluded and the toast is not" ships.
#
# Separate from the C++ block above because that block's regexes are C++-specific
# (`\bnew\s+`, `catch(...)`) and would misfire on Python prose.
# --------------------------------------------------------------------------
$pythonSources = Get-ChildItem -Path (Join-Path $repoRoot 'gui') -Recurse -Include *.py -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '__pycache__' }

$overlayRules = @(
    @{
        Pattern = '\bWDA_MONITOR\b'
        Allowed = @('framecapture_gui\overlay\exclusion.py', 'tests\test_overlay_exclusion.py')
        Reason  = 'WDA_MONITOR paints a black rectangle into every capture; use WDA_EXCLUDEFROMCAPTURE'
    }
    @{
        Pattern = '\bSetWindowDisplayAffinity\b'
        # The test is exempt because it is *about* display affinity -- it names the API
        # in its docstring to explain what it is measuring. Matching on prose as well as
        # code is deliberate: a rule that only looked at calls would miss a copy-paste
        # that had not been wired up yet, and the exemption list is two entries long.
        Allowed = @('framecapture_gui\overlay\exclusion.py', 'tests\test_overlay_exclusion.py')
        Reason  = 'call exclusion.exclude_from_capture() -- one module owns display affinity'
    }
)

$overlayViolations = @()
foreach ($file in $pythonSources) {
    # `@(...)` because `Get-Content` returns a bare string for a one-line file and
    # `$null` for an empty one, neither of which has a `.Count` -- which is a
    # PowerShell footgun this script hit the first time it ran.
    $lines = @(Get-Content -LiteralPath $file.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        foreach ($rule in $overlayRules) {
            if ($lines[$i] -notmatch $rule.Pattern) { continue }
            $exempt = $false
            foreach ($allowed in $rule.Allowed) {
                if ($file.FullName.EndsWith($allowed)) { $exempt = $true; break }
            }
            if (-not $exempt) {
                $overlayViolations += "{0}:{1}: {2}`n    {3}" -f $file.FullName, ($i + 1), $rule.Reason, $lines[$i].Trim()
            }
        }
    }
}

if ($overlayViolations) {
    Write-Host 'Overlay exclusion rules FAILED:' -ForegroundColor Red
    $overlayViolations | ForEach-Object { Write-Host $_ }
    $failed = $true
}
else {
    Write-Host 'Overlay exclusion rules: clean'
}

# --------------------------------------------------------------------------
# ruff + mypy (CLAUDE.md §4: "3.11+, PySide6, ruff + mypy --strict")
#
# Both read their configuration from pyproject.toml at the repo root, so what runs
# here and what an editor runs are the same thing.
#
# Skipped -- with a warning, never silently -- when the GUI venv is absent. The engine
# half of this script is what a C++-only contributor needs, and failing their lint run
# because they have no Python would push them to stop running it at all. The warning is
# what stops "skipped" from reading as "passed".
# --------------------------------------------------------------------------

$guiPython = Join-Path $env:LOCALAPPDATA 'FrameCapture\tools\guivenv\Scripts\python.exe'
$pythonSources = Get-ChildItem -Path (Join-Path $repoRoot 'gui') -Recurse -Include *.py -File -ErrorAction SilentlyContinue

if (-not (Test-Path $guiPython)) {
    Write-Skip 'ruff/mypy SKIPPED: no GUI venv. Run scripts\bootstrap.ps1.'
} elseif (-not $pythonSources) {
    Write-Skip 'ruff/mypy SKIPPED: no Python sources under gui/'
} else {
    Write-Host "python: $guiPython ($($pythonSources.Count) files)"

    # ruff. `--fix` under -Fix, mirroring what -Fix means for clang-format.
    $ruffArgs = @('-m', 'ruff', 'check', (Join-Path $repoRoot 'gui'))
    if ($Fix) { $ruffArgs += '--fix' }
    & $guiPython @ruffArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'ruff FAILED' -ForegroundColor Red
        $failed = $true
    } else {
        Write-Host 'ruff: clean'
    }

    # ruff's formatter, which is to Python what clang-format is to the C++ half.
    $formatArgs = @('-m', 'ruff', 'format', (Join-Path $repoRoot 'gui'))
    if (-not $Fix) { $formatArgs += '--check' }
    & $guiPython @formatArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'ruff format FAILED (run with -Fix)' -ForegroundColor Red
        $failed = $true
    }

    # mypy --strict. `strict = true` lives in pyproject.toml; passing the flag here as
    # well would mean two places to change it and one of them getting missed.
    & $guiPython -m mypy
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'mypy FAILED' -ForegroundColor Red
        $failed = $true
    } else {
        Write-Host 'mypy: clean'
    }
}

if ($failed) { exit 1 }
Write-Host 'Lint clean.' -ForegroundColor Green
