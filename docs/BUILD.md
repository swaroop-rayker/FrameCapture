# Building FrameCapture

> **Scope.** This document currently covers what exists: the C++ engine through
> milestone M0a. Sections for the Python GUI (`pytest gui/tests`), `run-dev.ps1`,
> `package.ps1` and the installer are absent because those scripts do not exist
> yet — they land with their milestones. Per SPEC.md §22 this file is a merge gate:
> if you add a build step or hit a new build failure, it goes here in the same
> commit.

Windows only (SPEC.md §2.2 item 2). There is no Linux or macOS build to fix.

---

## 1. Prerequisites

| Component | Minimum | Verified on the reference rig |
| --- | --- | --- |
| Visual Studio (Build Tools is enough — no IDE required) | 2022 17.8+ with the **Desktop development with C++** workload | Build Tools 2026, `18.8.12009.203`, MSVC `14.51.36231` |
| Windows SDK | 10.0.22621+ | 10.0.26100.0 |
| CMake | 3.25+ | 4.3.1 (bundled with the VS install) |
| Git | any recent | 2.55.0 |
| Python | 3.11+ (not needed until the GUI milestone) | 3.14.6 |

**You do not need CMake on `PATH`.** The VS install ships one at:

```
<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

**You do not need a Developer Command Prompt.** The presets deliberately omit a
`generator`, so CMake picks the default Visual Studio generator and locates MSVC
itself. A plain PowerShell works. (If you *are* in a Developer Prompt with
`CMAKE_GENERATOR=Ninja` set, the presets still work — they set both
`CMAKE_BUILD_TYPE` and `CMAKE_CONFIGURATION_TYPES`.)

**Disk:** budget ~2 GB. Measured after a first full build:

| Path | Size |
| --- | --- |
| `vcpkg/` (clone + downloads + buildtrees) | 1.3 GB |
| `build/` (all three presets + `vcpkg_installed`) | 0.2 GB |
| `%LOCALAPPDATA%\vcpkg\archives` (binary cache) | ~35 MB |

---

## 2. First build

```powershell
.\scripts\bootstrap.ps1
```

Clones vcpkg at the `builtin-baseline` pinned in `vcpkg.json` and bootstraps it.
Idempotent. It also probes for the certificate-revocation problem in §4.1 and
prints the fix if your machine has it — **read its output before continuing.**

```powershell
cmake --preset windows-msvc-release
```

⏱ **The first configure takes ~13 minutes** (measured: 773 s). Almost all of that
is vcpkg building FFmpeg from source, twice (debug and release). It is not hung —
see §4.5. Subsequent configures take ~15 s.

```powershell
cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release --output-on-failure
```

Verify by hand:

```powershell
.\build\windows-msvc-release\bin\framecapture-engine.exe
```

It prints the linked version of every dependency and exits 0.

---

## 3. Everyday commands

| Task | Command |
| --- | --- |
| Configure | `cmake --preset windows-msvc-release` |
| Build | `cmake --build --preset windows-msvc-release --parallel` |
| Test (CPU tier) | `ctest --preset windows-msvc-release --output-on-failure` |
| Test (GPU tier) | `ctest --preset windows-msvc-release -L gpu` |
| Lint | `.\scripts\lint.ps1` (add `-Fix` to reformat in place) |

Presets: `windows-msvc-debug`, `windows-msvc-relwithdebinfo`, `windows-msvc-release`.
All three share one dependency tree at `build/vcpkg_installed`, so switching between
them costs a configure, not another FFmpeg build.

### Spikes

Throwaway diagnostics under `scripts/spikes/` build standalone and are never part of
the engine build graph or `ctest`. They consume the already-installed dependency
tree, so configure the main build at least once first.

```powershell
cmake -S scripts/spikes/amf_zerocopy -B build/spike-amf
cmake --build build/spike-amf --config Release
```

---

## 4. Common build failures

### 4.1 `curl operation failed with error code 35 (SSL connect error)`

**By far the most likely failure on a fresh machine.** Every vcpkg download fails
and the diagnostic blames your proxy, which is almost always wrong.

```
A suitable version of cmake was not found (required v4.4.0).
Downloading https://github.com/Kitware/CMake/releases/download/v4.4.0/... 
error: curl operation failed with error code 35 (SSL connect error).
error: Not a transient network error, won't retry download from ...
note: If you are using a proxy, please ensure your proxy settings are correct.
```

**Cause — not a proxy.** vcpkg fetches over schannel with certificate revocation
checking enabled. If the machine cannot reach the CRL/OCSP responder, schannel
fails the handshake with `CRYPT_E_NO_REVOCATION_CHECK` (`0x80092012`) and curl
surfaces it as the generic error 35. Confirm it in one line:

```powershell
curl.exe -sSI -o NUL https://github.com
```

If that fails and `curl.exe -sSI --ssl-no-revoke -o NUL https://github.com`
succeeds, this is your problem.

**Fix.** Route vcpkg's fetches through the system curl with revocation checking
disabled *for those requests only*, using vcpkg's scripted asset source. Use the
helper in the repo rather than an inline curl — it also handles §4.5's dead mirror,
which an inline command cannot:

```powershell
$env:X_VCPKG_ASSET_SOURCES = "x-script,$PWD\scripts\fetch-asset.cmd {dst} {url}"
```

Set it in the shell you configure from, before `cmake --preset`. To make it stick,
add that line to your PowerShell profile (`$PROFILE`) rather than using `setx`.

This changes no system or security setting and does not disable revocation checking
for anything else on the machine. It is a build-host workaround; **do not** add it
to CI without deciding whether CI should be fetching from unvalidated endpoints.

**What does not work** — all three were tried, save yourself the time:

| Attempt | Why it fails |
| --- | --- |
| `CURL_HOME` pointing at a dir with a `_curlrc` containing `ssl-no-revoke` | Fixes the *system* `curl.exe`, but vcpkg statically links libcurl and never reads a curlrc. |
| A shim `curl.exe` earlier on `PATH` that injects `--ssl-no-revoke` | Same reason — vcpkg does not shell out to `curl.exe` for its own downloads. |
| Setting `HTTP_PROXY` / `HTTPS_PROXY` as the error message suggests | There is no proxy involved. The message is a red herring. |

### 4.2 `vcpkg was not found`

```
CMake Error: vcpkg was not found.
Run .\scripts\bootstrap.ps1 from the repository root, or set VCPKG_ROOT ...
```

**Cause.** `cmake/VcpkgSetup.cmake` looks for `<repo>/vcpkg`, then `$env:VCPKG_ROOT`.
Neither exists. **Fix.** Run `.\scripts\bootstrap.ps1`.

Note that a pre-existing `VCPKG_ROOT` install is *used but not pinned* to our
baseline. For a reproducible build, prefer the in-repo clone:
`.\scripts\bootstrap.ps1 -ForceLocalVcpkg`.

### 4.3 `A suitable version of cmake was not found (required v4.4.0)`

**Not an error on its own.** vcpkg downloads and uses its own CMake for building
ports, independent of the one that invoked it. Yours does not need to match. If it
is followed by a download failure, the real problem is §4.1.

### 4.5 `the asset cache script returned nonzero exit code 22` / automake 502

Appears while building `vcpkg-make`, which is pulled in by `x264` (and therefore by
the `gpl` feature added in M6):

```
Trying to download automake-1.17.tar.gz using asset cache script
curl: (22) The requested URL returned error: 502
error: the asset cache script returned nonzero exit code 22
error: curl operation failed with error code 35 (SSL connect error).
```

**Cause — two problems stacked.** vcpkg's automake port fetches from
`ftpmirror.gnu.org`, a redirector that has been returning 502. When the asset
script fails, vcpkg falls back to its *own* downloader for the alternate mirrors —
straight back into §4.1's revocation failure, which is why the log shows error 35
underneath the 502.

**Fix.** `scripts/fetch-asset.cmd` rewrites the host to `ftp.gnu.org` before
fetching, and uses the system curl with `--ssl-no-revoke`. Because vcpkg verifies
the SHA-512 after the download, a host rewrite cannot substitute different content;
only the path is load-bearing, and the script never rewrites paths.

Confirm which mirrors are reachable before assuming it is still the cause:

```powershell
curl.exe -sS -o NUL -w "%{http_code}`n" --ssl-no-revoke -L -I https://ftp.gnu.org/gnu/automake/automake-1.17.tar.gz
```

### 4.4 `clang-format not found` from `lint.ps1`

`lint.ps1` searches, in order: `PATH`, the Visual Studio LLVM directory,
`C:\Program Files\LLVM`, and finally
`%LOCALAPPDATA%\FrameCapture\tools\lintvenv\Scripts`.

The last of those is the recommended install, because it does not depend on an
optional Visual Studio component being present and it pins the tools to this
project:

```powershell
python -m venv "$env:LOCALAPPDATA\FrameCapture\tools\lintvenv"
& "$env:LOCALAPPDATA\FrameCapture\tools\lintvenv\Scripts\python.exe" -m pip install clang-format clang-tidy
```

Either of these also works, and `lint.ps1` will find them first:

- Visual Studio Installer → individual components → **C++ Clang tools for Windows**
- `winget install LLVM.LLVM`

Do not put the venv in a temp or scratch directory. It will be garbage-collected
and the gate will start reporting `clang-format not found` — which `lint.ps1`
treats as a warning, not a failure.

### 4.4a Getting clang-tidy running

`clang-tidy` needs a `compile_commands.json`, which the Visual Studio generator
cannot emit. Generate one once with Ninja, from a **Developer PowerShell** (Ninja
does not locate MSVC by itself):

```powershell
cmake --preset windows-msvc-debug -G Ninja -B build/tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

`lint.ps1` discovers any `compile_commands.json` under `build/` automatically — no
configuration needed. It then checks every file in the database in parallel, which
takes roughly two minutes; serially it takes over ten, because each translation unit
re-parses toml++, spdlog and gtest.

**Rerun that command whenever you add a `.cpp` file.** clang-tidy can only analyse
files listed in the database, so a stale one silently shrinks the gate's coverage.
`lint.ps1` guards against this: it fails and names the missing files rather than
reporting clean. If you see

```
clang-tidy FAILED: the compile database is stale.
```

that is what it means — reconfigure `build/tidy` and run it again. This is
BUG-005 in `ENGINEERING_LOG.md`; the gate had been silently skipping two
milestones' worth of source before the check existed.

Pass `-SkipTidy` to skip it for a fast formatting-only check.

Verify the config itself with `clang-tidy --verify-config`.

### 4.5 The first configure looks hung

It is not. vcpkg is building FFmpeg from source and prints nothing to the CMake
console while it works. Watch actual progress instead:

```powershell
Get-Content .\vcpkg\buildtrees\ffmpeg\build-x64-windows-rel-out.log -Tail 5 -Wait
```

Ports appear in `vcpkg/packages/` as they complete. Expect ~13 minutes total on a
7840HS. If a port genuinely fails, vcpkg prints the path to its `*-err.log`.

> Beware `build/<preset>/vcpkg-manifest-install.log` — it is **not** truncated
> between runs. A stale error in it from a previous failed attempt looks exactly
> like a current one. Check its timestamp.

### 4.6 FFmpeg rebuilds unexpectedly (~13 min)

**Cause.** Anything that changes the overlay port's ABI hash: editing
`ports/ffmpeg/portfile.cmake`, `ports/ffmpeg/framecapture-whitelist.cmake`, the
`builtin-baseline` in `vcpkg.json`, or the selected feature list.

This is correct behaviour — the whitelist determines which codecs exist in the
binary, so changing it *must* rebuild. Just know that touching the whitelist is a
13-minute operation, not a 15-second one. The binary cache in
`%LOCALAPPDATA%\vcpkg\archives` means reverting the change is fast.

---

## 5. Cleaning

| Goal | Command |
| --- | --- |
| Rebuild the engine only | `Remove-Item -Recurse -Force build\windows-msvc-release` |
| Rebuild dependencies too | also remove `build\vcpkg_installed` |
| Reclaim ~0.6 GB, keep everything working | `Remove-Item -Recurse -Force vcpkg\buildtrees` |
| Full reset | remove `build\` and `vcpkg\`, then re-run `bootstrap.ps1` |

`build/` and `vcpkg/` are both gitignored; neither is ever committed.
