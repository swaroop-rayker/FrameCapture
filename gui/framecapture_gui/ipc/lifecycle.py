"""The GUI's half of SPEC.md §3.1's process contract.

The GUI is the parent in that contract, so this is where the Job Object lives:

* create a **named** job with ``JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE``;
* spawn the engine **suspended**, assign it to the job, then resume;
* hand it the job's name and the pipe session id through the environment;
* heartbeat at §3.1's 1 s interval.

``ctypes`` rather than ``pywin32``: three Win32 calls do not justify a dependency the
installer would then have to carry, and ``ctypes`` is in the standard library.

**Why the spawn is suspended.** A process created running can execute -- and spawn
children of its own -- in the window before ``AssignProcessToJobObject`` lands, and
anything it spawned in that window is outside the job and therefore outside the orphan
guarantee. ``CREATE_SUSPENDED`` closes the window rather than narrowing it.

The engine's side of all this is ``engine/core/ipc/lifecycle.h``, including the
conflict inside §3.1 that decides whether the engine survives the GUI long enough to
finalize. That decision is the engine's; nothing here needs to change if it is revisited.
"""

from __future__ import annotations

import ctypes
import logging
import os
import secrets
import subprocess
import threading
from collections.abc import Callable
from ctypes import wintypes
from pathlib import Path

_log = logging.getLogger(__name__)

#: SPEC.md §3.1's heartbeat interval and timeout.
HEARTBEAT_INTERVAL_S = 1.0
HEARTBEAT_TIMEOUT_S = 5.0

#: The environment variables the engine reads. Must match ``fc::ipc::kJobNameEnvVar``
#: and ``kSessionEnvVar``.
JOB_NAME_ENV = "FC_ENGINE_JOB"
SESSION_ENV = "FC_ENGINE_SESSION"

_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
_JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
_CREATE_SUSPENDED = 0x00000004

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class _IoCounters(ctypes.Structure):
    _fields_ = [
        ("ReadOperationCount", ctypes.c_ulonglong),
        ("WriteOperationCount", ctypes.c_ulonglong),
        ("OtherOperationCount", ctypes.c_ulonglong),
        ("ReadTransferCount", ctypes.c_ulonglong),
        ("WriteTransferCount", ctypes.c_ulonglong),
        ("OtherTransferCount", ctypes.c_ulonglong),
    ]


class _BasicLimitInformation(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", wintypes.LARGE_INTEGER),
        ("PerJobUserTimeLimit", wintypes.LARGE_INTEGER),
        ("LimitFlags", wintypes.DWORD),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", wintypes.DWORD),
        ("Affinity", ctypes.POINTER(ctypes.c_ulong)),
        ("PriorityClass", wintypes.DWORD),
        ("SchedulingClass", wintypes.DWORD),
    ]


class _ExtendedLimitInformation(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", _BasicLimitInformation),
        ("IoInfo", _IoCounters),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


class JobObjectError(Exception):
    """The orphan guarantee could not be established."""


def new_session_id() -> str:
    """A fresh id for the control pipe's name.

    Hex rather than a GUID's punctuation: it goes straight into a pipe name and a job
    name, and both are cleaner without braces and dashes.
    """
    return secrets.token_hex(16)


class EngineJob:
    """A named Job Object that kills its members when the last handle closes."""

    def __init__(self, name: str | None = None) -> None:
        self.name = name or f"framecapture-job-{os.getpid()}-{secrets.token_hex(4)}"
        self._handle: int | None = None

    def create(self) -> None:
        _kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        _kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        handle = _kernel32.CreateJobObjectW(None, f"Local\\{self.name}")
        if not handle:
            raise JobObjectError(f"CreateJobObject failed ({ctypes.get_last_error()})")

        limits = _ExtendedLimitInformation()
        # SPEC.md §3.1's one required limit, and the whole of the orphan guarantee:
        # everything in the job dies when the last handle to it closes.
        limits.BasicLimitInformation.LimitFlags = _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE

        _kernel32.SetInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        ok = _kernel32.SetInformationJobObject(
            handle,
            _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
            ctypes.byref(limits),
            ctypes.sizeof(limits),
        )
        if not ok:
            _kernel32.CloseHandle(handle)
            raise JobObjectError(f"SetInformationJobObject failed ({ctypes.get_last_error()})")

        self._handle = handle

    def assign(self, process_handle: int) -> None:
        if self._handle is None:
            raise JobObjectError("the job has not been created")
        _kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        if not _kernel32.AssignProcessToJobObject(self._handle, process_handle):
            raise JobObjectError(f"AssignProcessToJobObject failed ({ctypes.get_last_error()})")

    def close(self) -> None:
        """Release the job handle.

        If this was the last handle, every process in the job is terminated by the
        kernel -- which is the point, and is what makes an orphaned engine impossible
        rather than merely unlikely.
        """
        if self._handle is not None:
            _kernel32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> EngineJob:
        self.create()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class EngineProcess:
    """A spawned engine, inside a job, with its session id."""

    def __init__(self, executable: Path, session_id: str | None = None) -> None:
        self.executable = executable
        self.session_id = session_id or new_session_id()
        self.job = EngineJob()
        self.popen: subprocess.Popen[bytes] | None = None

    def start(self) -> None:
        if not self.executable.is_file():
            raise JobObjectError(f"no engine at {self.executable}")

        self.job.create()

        environment = dict(os.environ)
        environment[JOB_NAME_ENV] = self.job.name
        environment[SESSION_ENV] = self.session_id

        # Suspended, then assigned, then resumed -- see the module note.
        self.popen = subprocess.Popen(
            [str(self.executable), "--serve"],
            env=environment,
            creationflags=_CREATE_SUSPENDED | subprocess.CREATE_NO_WINDOW,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        try:
            # `Popen._handle` is the process HANDLE on Windows. Private, and there is no
            # public alternative -- `subprocess` exposes the pid but not the handle, and
            # re-opening by pid would introduce a window in which the pid could have been
            # recycled. Guarded by the `getattr` below rather than assumed.
            handle = getattr(self.popen, "_handle", None)
            if handle is None:
                raise JobObjectError("subprocess.Popen exposed no process handle on this interpreter")
            self.job.assign(int(handle))
        except JobObjectError:
            # The engine is still suspended here, so killing it cannot leave a partly
            # started recording behind.
            self.popen.kill()
            self.job.close()
            raise

        # Only now. Everything before this point ran with the engine frozen at its entry
        # point, which is what makes the job assignment airtight.
        _resume_process_threads(self.popen.pid)

    @property
    def pid(self) -> int:
        return self.popen.pid if self.popen is not None else 0

    def alive(self) -> bool:
        return self.popen is not None and self.popen.poll() is None

    def wait(self, timeout_s: float) -> int | None:
        if self.popen is None:
            return None
        try:
            return self.popen.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            return None

    def close(self) -> None:
        """Release the job handle, reaping the engine if it is still there."""
        self.job.close()


_THREAD_SUSPEND_RESUME = 0x0002
_TH32CS_SNAPTHREAD = 0x00000004


class _ThreadEntry32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ThreadID", wintypes.DWORD),
        ("th32OwnerProcessID", wintypes.DWORD),
        ("tpBasePri", wintypes.LONG),
        ("tpDeltaPri", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
    ]


def _resume_process_threads(pid: int) -> None:
    """Resume every thread of ``pid``.

    A freshly ``CREATE_SUSPENDED`` process has exactly one, so "every" is one -- but
    enumerating is the documented way to reach it, and resuming a set that happens to be
    a singleton is safer than assuming it always will be.
    """
    _kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snapshot = _kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPTHREAD, 0)
    if snapshot == wintypes.HANDLE(-1).value or not snapshot:
        raise JobObjectError(f"CreateToolhelp32Snapshot failed ({ctypes.get_last_error()})")

    try:
        entry = _ThreadEntry32()
        entry.dwSize = ctypes.sizeof(_ThreadEntry32)
        if not _kernel32.Thread32First(snapshot, ctypes.byref(entry)):
            raise JobObjectError("Thread32First found no threads")

        resumed = 0
        while True:
            if entry.th32OwnerProcessID == pid:
                _kernel32.OpenThread.restype = wintypes.HANDLE
                thread = _kernel32.OpenThread(_THREAD_SUSPEND_RESUME, False, entry.th32ThreadID)
                if thread:
                    _kernel32.ResumeThread(thread)
                    _kernel32.CloseHandle(thread)
                    resumed += 1
            if not _kernel32.Thread32Next(snapshot, ctypes.byref(entry)):
                break

        if resumed == 0:
            raise JobObjectError(f"no threads of pid {pid} could be resumed; the engine would never start")
    finally:
        _kernel32.CloseHandle(snapshot)


class Heartbeat:
    """The GUI's half of §3.1's heartbeat.

    Any inbound message is the engine's evidence that the GUI is alive -- SPEC.md §15.1
    has no ``heartbeat`` command and inventing one would extend a specified surface. A
    periodic ``get_stats`` is both the liveness signal and the thing a GUI showing stats
    would send anyway.
    """

    def __init__(self, beat: Callable[[], None], interval_s: float = HEARTBEAT_INTERVAL_S) -> None:
        self._beat = beat
        self._interval = interval_s
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="fc-heartbeat", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.wait(self._interval):
            try:
                self._beat()
            except Exception:
                # A failed beat means the engine is gone, which the controller discovers
                # on its own next command. Logged at debug: during shutdown this is the
                # expected outcome, and a warning per second would bury the real one.
                _log.debug("heartbeat failed", exc_info=True)

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None
