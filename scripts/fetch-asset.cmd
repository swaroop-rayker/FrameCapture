@echo off
rem ---------------------------------------------------------------------------
rem vcpkg asset fetcher (docs/BUILD.md §4.1, §4.2)
rem
rem Invoked by vcpkg through X_VCPKG_ASSET_SOURCES as:
rem     fetch-asset.cmd <destination> <url>
rem
rem It exists for two reasons, both build-host problems rather than project ones:
rem
rem   1. Certificate revocation. vcpkg's built-in downloader uses schannel with
rem      revocation checking on, and a host that cannot reach the CRL/OCSP
rem      responder fails every handshake as the generic curl error 35. The system
rem      curl with --ssl-no-revoke gets through. Scoped to these fetches only; no
rem      system or security setting is changed.
rem
rem   2. Dead mirrors. vcpkg's port for automake points at ftpmirror.gnu.org,
rem      which is a redirector that has been returning 502. When the asset script
rem      fails, vcpkg falls back to its *own* downloader for the alternate URLs --
rem      straight back into problem 1 -- so retrying inside the script is the only
rem      place a substitution can help.
rem
rem Rewrites are listed one per line below so adding one is obvious. Keep them
rem host-only: rewriting a path would silently fetch different content, and the
rem SHA-512 check vcpkg applies afterwards is what makes a host rewrite safe.
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "DST=%~1"
set "URL=%~2"

if "%DST%"=="" goto :usage
if "%URL%"=="" goto :usage

rem --- host rewrites ---------------------------------------------------------
set "URL=!URL:ftpmirror.gnu.org=ftp.gnu.org!"

curl.exe -L --ssl-no-revoke --fail --retry 3 --retry-delay 2 --create-dirs --output "%DST%" "%URL%"
exit /b %ERRORLEVEL%

:usage
echo usage: fetch-asset.cmd ^<destination^> ^<url^> 1>&2
exit /b 2
