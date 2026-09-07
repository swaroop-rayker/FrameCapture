"""The control protocol's vocabulary (SPEC.md §15.1).

The Python mirror of ``engine/core/ipc/protocol.h``. Names are the wire spellings, so a
misspelling here is a failed lookup rather than a message the engine silently rejects.

SPEC.md §15.1's compatibility rule is a property of the *reader*, and this side has to
honour it too: an engine one minor version ahead may send fields and events this build
has no name for, and neither may be fatal. ``Event.from_wire`` therefore returns ``None``
for an unrecognised event rather than raising, and nothing here enumerates the fields of
a response.
"""

from __future__ import annotations

from enum import StrEnum

#: This build's protocol version, and the major it will accept from an engine.
PROTOCOL_VERSION = "1.0"
PROTOCOL_MAJOR = 1


class Command(StrEnum):
    """SPEC.md §15.1's command set, verbatim and complete.

    A ``str`` enum so the value *is* the wire spelling -- there is no second table
    mapping one to the other, and therefore nothing to drift.
    """

    HELLO = "hello"
    GET_SOURCES = "get_sources"
    GET_DEVICES = "get_devices"
    GET_GPU_TOPOLOGY = "get_gpu_topology"
    CONFIGURE = "configure"
    START_PREVIEW = "start_preview"
    STOP_PREVIEW = "stop_preview"
    START_RECORD = "start_record"
    STOP_RECORD = "stop_record"
    PAUSE_RECORD = "pause_record"
    RESUME_RECORD = "resume_record"
    GET_STATS = "get_stats"
    GET_HEALTH = "get_health"
    SET_LOG_LEVEL = "set_log_level"
    RECOVER = "recover"
    SHUTDOWN = "shutdown"

    # Beyond SPEC.md §15.1's sixteen; added 2026-08-03 so settings can persist. §17
    # makes the engine the only writer of `config.toml` -- it already implements the
    # atomic write, the migration chain and unknown-key preservation -- and these two
    # are how the GUI reads and drives it. Needs the owner's pen on §15.1.
    GET_CONFIG = "get_config"
    SAVE_CONFIG = "save_config"


class Event(StrEnum):
    """SPEC.md §15.1's unsolicited engine → GUI events."""

    STATE_CHANGED = "state_changed"
    STATS = "stats"
    WARNING = "warning"
    ERROR = "error"
    GPU_MIGRATED = "gpu_migrated"
    AUDIO_DEVICE_MIGRATED = "audio_device_migrated"
    DEGRADATION_CHANGED = "degradation_changed"
    SEGMENT_ROLLED = "segment_rolled"
    RECORDING_FINALIZED = "recording_finalized"

    # Beyond SPEC.md §15.1's nine; added by M9.6 so the GUI can show what
    # `stop_record` is doing during the seconds it takes. Additive under §15.1's
    # compatibility rule -- an engine that does not send it costs the progress bar its
    # detail, not its correctness, because `recording_finalized` still ends the wait.
    FINALIZE_PROGRESS = "finalize_progress"

    @classmethod
    def from_wire(cls, name: str) -> Event | None:
        """Parse an event name, or ``None`` if this build has never heard of it.

        ``None`` rather than an exception: SPEC.md §15.1 makes new features additive, so
        an engine a minor version ahead emitting an event this build cannot name must
        not take the GUI down. The caller ignores what it does not understand.
        """
        try:
            return cls(name)
        except ValueError:
            return None


class RecordingState(StrEnum):
    """Recording states, as carried by ``state_changed``.

    SPEC.md §15.1: "Paused is a `state_changed` value, not an event of its own -- the GUI
    must render it as a distinct state (§16.5), because a paused recording that looks
    like a running one loses footage silently."
    """

    IDLE = "idle"
    STARTING = "starting"
    RECORDING = "recording"
    PAUSED = "paused"
    STOPPING = "stopping"
    FAULTED = "faulted"

    #: Not an engine state. The GUI's own, for when there is no engine to ask -- SPEC.md
    #: §16.1: "fully functional and honest when the engine is down: greyed controls +
    #: a clear ENGINE OFFLINE state, plus a restart action."
    OFFLINE = "offline"

    @classmethod
    def from_wire(cls, name: str) -> RecordingState:
        """Parse a state name, falling back to ``FAULTED``.

        Deliberately *not* ``IDLE``: an unrecognised state is a GUI that does not know
        what the engine is doing, and showing "idle" would invite a user to press start
        on a recording that may already be running. Faulted is visible and safe.
        """
        try:
            return cls(name)
        except ValueError:
            return cls.FAULTED


#: SPEC.md §15.1: "Requests time out at 5 s (except `stop_record`, 30 s, since
#: finalization is legitimately slow)."
DEFAULT_TIMEOUT_S = 5.0
STOP_RECORD_TIMEOUT_S = 30.0

#: `recover` gets `stop_record`'s budget, and §15.1's sentence has to be read to mean it.
#:
#: The command runs `mux::recover`, which for MP4 is *the same lossless remux a clean
#: stop performs* -- BUG-046 measured that at ~3.3 s of remux plus ~1 s of validate for a
#: 1.2 GB recording. §15.1 names only `stop_record` because `recover` had no caller when
#: it was written; leaving it at 5 s would mean the recovery path times out on every
#: recording large enough to be worth recovering, which is all of them.
#:
#: Flagged rather than assumed: §15.1's wording needs the owner's pen (M9.6 Phase 6).
RECOVER_TIMEOUT_S = 30.0

#: The commands whose work is bounded by the disk rather than by a lock.
_SLOW_COMMANDS = {Command.STOP_RECORD: STOP_RECORD_TIMEOUT_S, Command.RECOVER: RECOVER_TIMEOUT_S}


def timeout_for(command: Command) -> float:
    return _SLOW_COMMANDS.get(command, DEFAULT_TIMEOUT_S)


def pipe_path_for(session_id: str) -> str:
    r"""``\\.\pipe\framecapture-{session_guid}``.

    One formatter, matching ``fc::ipc::pipe_path_for``. Two independent format strings is
    how a rename breaks one side only.
    """
    return rf"\\.\pipe\framecapture-{session_id}"
