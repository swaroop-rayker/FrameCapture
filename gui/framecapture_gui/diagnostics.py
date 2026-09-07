"""What a user hands a maintainer (SPEC.md §18).

Two things live here, and both are deliberately free of Qt so they can be tested
without a display:

* `format_summary` — the text behind Edit ▸ Copy diagnostics summary. Versions,
  adapters, encoder, the last error. The thing that gets pasted into a bug report.
* `write_bundle` — §18's "A 'Export diagnostic bundle' button zips logs + config +
  last minidump for manual sharing."

**Neither ever sends anything anywhere.** §18: "Telemetry is local-only. No network
transmission, ever." The bundle is a file the user chooses the location of, and the
summary goes to the clipboard. There is no third option here, and there must not be
one.

Everything is defensive about missing inputs on purpose. The moment a user most wants
diagnostics is the moment the engine is not answering, and a summary that refuses to
render without a live engine is a summary that is absent exactly when it is needed.
`None` for any engine-supplied section is a supported input and produces a line saying
so, not an exception and not a blank.
"""

from __future__ import annotations

import os
import zipfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

#: The engine's crash artefacts share this stem shape: `crash_<key>.dmp`,
#: `crash_<key>.log`, `crash_report_<key>.json` (`crash_handler.cpp`). The key is what
#: ties one crash's three files together, and it is why the bundle takes a *set* rather
#: than a single dump -- a minidump with no `crash_report.json` beside it is missing the
#: state-machine position that says what the engine was doing.
_DUMP_PREFIX = "crash_"
_DUMP_SUFFIX = ".dmp"

#: Where each kind of file lands inside the zip. Directories rather than a flat list so
#: that an unpacked bundle is navigable, and so a log named the same as a crash log
#: cannot collide.
_LOGS_DIR = "logs"
_CONFIG_DIR = "config"
_CRASHES_DIR = "crashes"
SUMMARY_MEMBER = "summary.txt"


def data_directory() -> Path:
    r"""``%LOCALAPPDATA%\FrameCapture`` (SPEC.md §18).

    The GUI's mirror of `log::default_log_directory`'s parent. Duplicated rather than
    asked for over IPC because the folder has to be openable with no engine running,
    which is the case a user most often wants the logs in.
    """
    return Path(os.environ.get("LOCALAPPDATA", "")) / "FrameCapture"


def default_log_directory() -> Path:
    """§18's rotating file sink location."""
    return data_directory() / "logs"


def default_crash_directory() -> Path:
    """Where `crash_handler.cpp` puts minidumps -- a sibling of the log directory."""
    return data_directory() / "crashes"


@dataclass(frozen=True)
class BundleReport:
    """What `write_bundle` actually put in the file."""

    path: Path
    members: list[str] = field(default_factory=list)
    #: Names of the inputs that were asked for and were not there. Reported rather than
    #: hidden: "the bundle has no logs" is a fact the person receiving it needs, and a
    #: silently smaller zip does not say it.
    missing: list[str] = field(default_factory=list)

    @property
    def size_bytes(self) -> int:
        return self.path.stat().st_size if self.path.is_file() else 0


def latest_crash_files(crash_directory: Path) -> list[Path]:
    """The newest minidump and everything written alongside it, or an empty list.

    Newest by modification time rather than by the timestamp in the filename: the name's
    stamp is local time and would sort wrongly across a DST boundary, which is a
    once-a-year bug that hides for six months.
    """
    if not crash_directory.is_dir():
        return []
    dumps = [p for p in crash_directory.iterdir() if p.is_file() and p.suffix.lower() == _DUMP_SUFFIX]
    if not dumps:
        return []
    newest = max(dumps, key=lambda p: p.stat().st_mtime)

    key = newest.stem
    if key.startswith(_DUMP_PREFIX):
        key = key[len(_DUMP_PREFIX) :]
    siblings = [p for p in crash_directory.iterdir() if p.is_file() and key in p.name]
    return sorted(siblings)


def write_bundle(
    destination: Path,
    *,
    summary: str,
    log_directory: Path | None = None,
    config_path: Path | None = None,
    crash_directory: Path | None = None,
) -> BundleReport:
    """Zip the diagnostics into `destination`, and say what went in.

    Deflated rather than stored: logs are text and compress by an order of magnitude,
    and the whole point of the bundle is that a user can attach it to something.

    The summary is written into the zip as well as being offered on the clipboard. A
    bundle that arrives without it is a pile of logs with no statement of what machine
    produced them.
    """
    members: list[str] = []
    missing: list[str] = []

    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        bundle.writestr(SUMMARY_MEMBER, summary)
        members.append(SUMMARY_MEMBER)

        if log_directory is not None:
            logs = sorted(p for p in log_directory.iterdir() if p.is_file()) if log_directory.is_dir() else []
            if not logs:
                missing.append(f"logs ({log_directory})")
            for entry in logs:
                member = f"{_LOGS_DIR}/{entry.name}"
                bundle.write(entry, member)
                members.append(member)

        if config_path is not None:
            if config_path.is_file():
                member = f"{_CONFIG_DIR}/{config_path.name}"
                bundle.write(config_path, member)
                members.append(member)
            else:
                missing.append(f"config ({config_path})")

        if crash_directory is not None:
            crashes = latest_crash_files(crash_directory)
            if not crashes:
                # Not a failure. A machine that has never crashed is the expected case,
                # and saying "no crash dumps" is more useful than an absent directory.
                missing.append("crash dump (none recorded)")
            for entry in crashes:
                member = f"{_CRASHES_DIR}/{entry.name}"
                bundle.write(entry, member)
                members.append(member)

    return BundleReport(path=destination, members=members, missing=missing)


# ---------------------------------------------------------------------------
# The summary
# ---------------------------------------------------------------------------


def _line(label: str, value: object) -> str:
    return f"{label}: {value}"


def _adapters(topology: Mapping[str, Any] | None) -> list[str]:
    """SPEC.md §5.1's topology, in the shape that makes the bug obvious.

    `owns_output` is marked explicitly because it is the single field that explains the
    black-frame class of defect: on a MUX-less laptop the display hangs off the iGPU,
    and a capture device created on the other adapter returns black with `S_OK`. A bug
    report that says which adapter owns the output has already answered the first
    question a maintainer would ask.
    """
    if topology is None:
        return ["  (unavailable — the engine did not answer)"]
    adapters = topology.get("adapters") or []
    if not adapters:
        return ["  (the engine reported no adapters)"]

    rows: list[str] = []
    for adapter in adapters:
        if not isinstance(adapter, Mapping):
            continue
        description = str(adapter.get("description", "unnamed adapter"))
        luid = adapter.get("luid", 0)
        owns = " — owns the display output" if adapter.get("owns_output") else ""
        rows.append(f"  {description} (LUID {luid}){owns}")
    return rows or ["  (the engine reported no adapters)"]


def _video(config: Mapping[str, Any]) -> str:
    fps = config.get("fps", "?")
    encoder = str(config.get("encoder", "?"))
    container = str(config.get("container", "?"))
    cqp = config.get("cqp", "?")
    return f"1920x1080 @{fps}, encoder {encoder}, CQP {cqp}, container {container}"


def _audio(config: Mapping[str, Any]) -> str:
    layout = str(config.get("channel_layout", "?"))
    bitrate = config.get("audio_bitrate_kbps", "?")
    device = str(config.get("audio_device", "")) or "default endpoint"
    multitrack = "on" if config.get("multitrack_enabled") else "off"
    return f"{device}, {layout}, {bitrate} kbps, multi-track {multitrack}"


def _health(health: Mapping[str, Any] | None) -> list[str]:
    if health is None:
        return [_line("Health", "unavailable — the engine did not answer")]
    rung = health.get("rung", 0)
    if "hardware_encoder" not in health:
        # `health_payload` returns `{"rung": 0}` and nothing else when no recording is
        # open. Saying "no recording" is honest; rendering zeros for every counter would
        # read as a recording that captured nothing.
        return [_line("Health", f"degradation rung {rung}, no recording open")]
    encoder = "hardware" if health.get("hardware_encoder") else "software (x264)"
    return [
        _line("Health", f"degradation rung {rung}, {encoder} encoder"),
        # A continuation of the line above, not a field of its own -- `_line` would put a
        # stray colon after the indent.
        f"  stalls {health.get('stall_episodes', 0)} (worst {health.get('worst_stall_ms', 0)} ms), "
        f"disk write p99 {health.get('disk_write_p99_ms', 0)} ms, "
        f"{health.get('disk_free_mb', 0)} MB free",
    ]


def format_summary(
    *,
    gui_version: str,
    engine_version: str,
    protocol: str,
    state: str,
    capabilities: Sequence[str],
    platform: str,
    config: Mapping[str, Any] | None = None,
    topology: Mapping[str, Any] | None = None,
    health: Mapping[str, Any] | None = None,
    last_error: str = "",
    now: datetime | None = None,
) -> str:
    """The pasteable summary. Never raises, whatever the engine did or did not say.

    The parameters are the *answers*, not the engine: this function does no I/O and
    holds no reference to anything live, which is what makes every branch of it -- and
    especially the engine-offline branch -- testable without hardware.
    """
    stamp = (now or datetime.now(UTC)).strftime("%Y-%m-%dT%H:%M:%SZ")

    lines = [
        f"FrameCapture diagnostics — {stamp}",
        _line("Versions", f"GUI {gui_version} · engine {engine_version} · IPC protocol {protocol}"),
        _line("Platform", platform),
        _line("Engine", state),
        _line("Capabilities", ", ".join(capabilities) if capabilities else "none reported"),
        "",
    ]

    if config is None:
        lines.append("Configuration: unavailable — the engine did not answer")
    else:
        lines.extend(
            [
                _line("Video", _video(config)),
                _line("Audio", _audio(config)),
                _line(
                    "Capture",
                    f"backend {config.get('capture_backend', '?')}, "
                    f"cursor {'on' if config.get('capture_cursor') else 'off'}, "
                    f"HDR tone-map {'on' if config.get('hdr_tonemap') else 'off'}",
                ),
                _line("Log level", config.get("log_level", "?")),
                _line("Config schema", config.get("schema_version", "?")),
                _line("Config file", config.get("config_path", "?")),
            ]
        )

    lines.extend(["", "Adapters:", *_adapters(topology), ""])
    lines.extend(_health(health))
    lines.append(_line("Last error", last_error or "none reported this session"))
    return "\n".join(lines)
