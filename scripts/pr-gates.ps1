#Requires -Version 5.1
<#
.SYNOPSIS
    Repository policy gates for CI (M9.6 plan §7.4, §7.5).

.DESCRIPTION
    Two kinds of check, both cheap, both things CLAUDE.md §10 already requires of a
    change and neither of which anything enforced until now:

      1. LICENSE must exist. CLAUDE.md §9 records that libx264 is in, `--enable-gpl`
         is set, and the distributed binary is therefore GPLv2 -- which obliges source
         availability for the distributed work. A repository that ships that with no
         licence file has an unanswered obligation, and the point of failing here is
         that the question cannot be quietly forgotten.

      2. Documentation pairs. "If you add a config key, it goes in docs/CONFIG.md in
         the same commit. If you add an FcError, it goes in docs/ERROR_CODES.md in the
         same commit." SPEC.md §22 calls the docs a merge-gate requirement; this is
         the gate.

    **The documentation checks read the diff, not the file list.** Triggering on "any
    file under engine/core/config/ changed" would fire on a comment fix and teach
    everyone to add a pointless CONFIG.md edit to shut it up -- a gate that cries wolf
    gets routed around, which leaves it worse than absent. So they fire on an *added*
    `KeySpec{...}` line or an *added* `X(NAME, code, ...)` error enumerator, which is
    what "adds a key" and "adds an error" actually look like in this codebase.

.PARAMETER BaseRef
    Derive everything from `git diff <BaseRef>...HEAD`. This is what CI passes.

.PARAMETER ChangedFiles
    An explicit list of repo-relative paths, instead of asking git. For tests.

.PARAMETER AddedConfigKeys
    Explicit list of config keys the change adds, instead of reading the diff. For tests.

.PARAMETER AddedErrorCodes
    Explicit list of error enumerators the change adds. For tests.

.EXAMPLE
    scripts\pr-gates.ps1 -BaseRef origin/main
#>
[CmdletBinding()]
param(
    [string]$BaseRef,
    [string[]]$ChangedFiles,
    [string[]]$AddedConfigKeys,
    [string[]]$AddedErrorCodes,
    # Check the detectors still recognise this codebase, and do nothing else.
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$failed = $false

# The two detectors, defined once and used by both the diff pass and -SelfTest.
#
# A gate whose pattern has quietly stopped matching reports clean forever, which is the
# worst thing a gate can do. -SelfTest is what stops that being discoverable only by
# someone adding a key and noticing nothing happened.
$script:ConfigKeyPattern = '^\+\s*KeySpec\{\s*"([^"]+)"'
$script:ErrorCodePattern = '^\+\s*X\(\s*([A-Z][A-Z0-9_]*)\s*,'

function Select-Matches {
    <#
        Every capture of $Pattern across $Lines.

        **Call sites must wrap this in `@()`.** PowerShell unrolls a returned empty
        array to `$null`, and `$null.Count` throws under `Set-StrictMode -Latest` -- so
        the no-match branch, which is the one that matters most here, crashed instead of
        reporting. Found by reverting the pattern to check the self-test could fail.
    #>
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [Parameter(Mandatory)][string]$Pattern
    )
    return @(
        @($Lines) |
            Where-Object { $_ -match $Pattern } |
            ForEach-Object { $Matches[1] }
    )
}

function Write-Fail {
    param([Parameter(Mandatory)][string]$Title, [string[]]$Detail)
    Write-Host "FAILED: $Title" -ForegroundColor Red
    foreach ($line in $Detail) { Write-Host "  $line" }
    $script:failed = $true
}

# `git` reports forward slashes on every platform; a caller on Windows may not. One
# spelling, so a comparison cannot fail on the separator alone.
function ConvertTo-RepoPath {
    param([string]$Path)
    return ($Path -replace '\\', '/').Trim()
}

# One list, however the caller spelled it.
#
# PowerShell's `-File` invocation binds `-ChangedFiles a,b` as the single string "a,b"
# rather than as two elements, and GitHub Actions hands file lists over as newline-
# separated text. Both are the natural way to call this, neither works without splitting
# here, and the failure is silent: the gate sees one path where there were forty and
# reports clean.
function Expand-List {
    param([AllowEmptyCollection()][string[]]$Values = @())
    return @(
        @($Values) |
            Where-Object { $_ } |
            ForEach-Object { $_ -split "[,`r`n]" } |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ }
    )
}

# ---------------------------------------------------------------------------
# -SelfTest: are the detectors still looking for something that exists?
# ---------------------------------------------------------------------------
#
# Both patterns match a *declaration form* rather than a filename, which is what makes
# the gate precise -- and also what makes it silently fragile. Reformat the KeySpec
# table, or replace the FcError X-macro, and the pattern matches nothing, the gate
# reports clean on every change forever, and nobody finds out until a config key ships
# undocumented.
#
# So the patterns are run against the real declarations, with every line treated as if
# it were added. If either finds nothing, this codebase no longer looks the way the gate
# believes it does.

if ($SelfTest) {
    $checks = @(
        @{ Path = 'engine/core/config/config_schema.cpp'; Pattern = $script:ConfigKeyPattern; What = 'config key' }
        @{ Path = 'engine/core/error/fc_error.h';         Pattern = $script:ErrorCodePattern; What = 'error code' }
    )

    foreach ($check in $checks) {
        $full = Join-Path $repoRoot $check.Path
        if (-not (Test-Path $full)) {
            Write-Fail "$($check.Path) does not exist" @(
                "The $($check.What) detector points at a file that has moved or been renamed."
            )
            continue
        }
        # Prefixed with '+' so the detector sees exactly what it sees in a diff. Using
        # a second, diff-free pattern here would test a pattern the gate does not use.
        $asAdded = Get-Content $full | ForEach-Object { "+$_" }
        $found = @(Select-Matches -Lines $asAdded -Pattern $check.Pattern)
        if ($found.Count -eq 0) {
            Write-Fail "the $($check.What) detector matches nothing in $($check.Path)" @(
                "Pattern: $($check.Pattern)",
                'The declaration form has changed and this gate is now vacuous: it would',
                'report clean on every change, including one that adds an undocumented one.'
            )
        }
        else {
            Write-Host "$($check.What) detector: $($found.Count) match(es) in $($check.Path)"
        }
    }

    if ($failed) {
        Write-Host ''
        Write-Host 'pr-gates -SelfTest FAILED' -ForegroundColor Red
        exit 1
    }
    Write-Host 'pr-gates -SelfTest: clean' -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# Where the change's contents come from
# ---------------------------------------------------------------------------

if ($BaseRef) {
    # `...` (three dots) is deliberate: it diffs against the merge base, so a PR is
    # judged on what it changed and not on what main did underneath it while it was
    # open. Two dots would demand a CONFIG.md edit from a branch whose only sin is
    # being behind.
    $range = "$BaseRef...HEAD"

    $names = & git -C $repoRoot diff --name-only $range
    if ($LASTEXITCODE -ne 0) {
        Write-Fail "could not diff against $BaseRef" @(
            'Fetch it first. In CI this means actions/checkout with fetch-depth: 0.'
        )
        exit 1
    }
    $ChangedFiles = @($names | Where-Object { $_ })

    # -U0: only the changed lines, so an added key is not confused with one that
    # happened to sit inside another hunk's context.
    $configDiff = & git -C $repoRoot diff -U0 $range -- 'engine/core/config/'
    $AddedConfigKeys = @(Select-Matches -Lines $configDiff -Pattern $script:ConfigKeyPattern)

    $errorDiff = & git -C $repoRoot diff -U0 $range -- 'engine/core/error/'
    $AddedErrorCodes = @(
        Select-Matches -Lines $errorDiff -Pattern $script:ErrorCodePattern |
            Where-Object { $_ -ne 'NONE' }
    )
}

$changed = @(Expand-List $ChangedFiles | ForEach-Object { ConvertTo-RepoPath $_ })
$addedKeys = @(Expand-List $AddedConfigKeys)
$addedErrors = @(Expand-List $AddedErrorCodes)

Write-Host "pr-gates: $($changed.Count) changed file(s), $($addedKeys.Count) new config key(s), $($addedErrors.Count) new error code(s)"

# ---------------------------------------------------------------------------
# Gate 1 -- LICENSE (plan §7.4)
# ---------------------------------------------------------------------------

$licence = @('LICENSE', 'LICENSE.txt', 'LICENSE.md', 'COPYING') |
    Where-Object { Test-Path (Join-Path $repoRoot $_) }

if ($licence) {
    Write-Host "LICENSE: $($licence[0])"
}
else {
    Write-Fail 'the repository has no LICENSE file' @(
        'CLAUDE.md §9: libx264 is in, --enable-gpl is set, and the distributed binary is',
        'therefore GPLv2. That obliges source availability for the distributed work and is',
        'incompatible with shipping under a proprietary licence.',
        '',
        'This gate is failing on purpose. It is not asking for a file to be created to make',
        'it pass -- it is asking for the licence decision to be recorded, which is the',
        "owner's to make (CLAUDE.md §9: do not guess).",
        '',
        'Add LICENSE at the repository root once that decision is made.'
    )
}

# ---------------------------------------------------------------------------
# Gate 2 -- documentation pairs (plan §7.5, SPEC.md §22, CLAUDE.md §10)
# ---------------------------------------------------------------------------

function Test-DocPair {
    param(
        # Not `Mandatory`: an empty array fails mandatory binding outright, so the
        # common case -- a change that adds no keys and no error codes -- would crash
        # the gate rather than pass it.
        [AllowEmptyCollection()][string[]]$Added = @(),
        [Parameter(Mandatory)][string]$Doc,
        [Parameter(Mandatory)][string]$What
    )

    if (-not $Added) { return }
    if ($changed -contains $Doc) {
        Write-Host "$Doc updated alongside $($Added.Count) new $What"
        return
    }
    Write-Fail "$($Added.Count) new $What, and $Doc was not touched" @(
        ($Added | ForEach-Object { "  $_" })
        '',
        "CLAUDE.md §10 requires the documentation in the same commit, and SPEC.md §22 makes",
        'it a merge-gate requirement rather than a habit.'
    )
}

Test-DocPair -Added $addedKeys   -Doc 'docs/CONFIG.md'      -What 'config key(s)'
Test-DocPair -Added $addedErrors -Doc 'docs/ERROR_CODES.md' -What 'error code(s)'

if ($failed) {
    Write-Host ''
    Write-Host 'pr-gates FAILED' -ForegroundColor Red
    exit 1
}

Write-Host 'pr-gates: clean' -ForegroundColor Green
exit 0
