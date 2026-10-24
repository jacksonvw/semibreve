"""Ownership of the daemon process.

The daemon and the window are separate processes for a real reason — the audio
callback must not be able to be stalled by Qt — but that split is an
implementation detail and it should never have been the user's problem. From
outside there is one application: launching it makes the sink appear, quitting
it makes the sink go away, and nothing is left behind.

So this owns `semibreved`:

* **start** it if it is not already there, when the app opens
* **restart** it if it dies — a PipeWire restart takes its nodes away and it
  exits deliberately, and the sink should come back on its own rather than the
  user discovering their output device has vanished
* **stop** it when the app quits, over D-Bus so it shuts down cleanly

The one case it deliberately does not own is a daemon it did not start: if one
was already running when the app opened — from a shell, or a systemd unit — it
is left alone at quit. Stopping something another owner started would be the
same discourtesy in the other direction.

Left alone is not the same as left gone, though. If a daemon like that goes
away and whatever started it does not bring it back within a few seconds, the
window starts one of its own, and owns that one. A second daemon refuses to
start, so the worst a race can do is have one of the two exit.
"""

from __future__ import annotations

import logging
import os
import shutil
import signal
import subprocess
import time
from pathlib import Path

from PySide6.QtCore import QObject, QTimer, Signal

from semibreve.services.daemon import DaemonService

log = logging.getLogger(__name__)

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
DAEMON = PROJECT_ROOT / "build" / "semibreved"
LOG_PATH = "/tmp/semibreved.log"

# Give up after this many restarts in a row. A daemon that cannot survive its
# own startup would otherwise be relaunched forever, and a fast loop is worse
# than an honest "it will not start".
MAX_RESTARTS = 3

# A daemon that ran this long before stopping did survive its startup, so its
# exit starts a new run of restarts instead of adding to the last one. Without
# it the count never reset, and the fourth unplugged output device of a session
# was taken for a daemon that will not start.
STABLE_SECONDS = 30

# How long a daemon the window did not start may be gone before the window
# starts its own: long enough for a service manager to have restarted it.
ADOPT_GRACE_SECONDS = 3


def _pactl(*args: str) -> str:
    """Run pactl, returning stdout. Empty on any failure — no audio operation
    here is worth an exception in a UI callback."""
    exe = shutil.which("pactl")
    if exe is None:
        return ""
    try:
        return subprocess.run([exe, *args], capture_output=True, text=True,
                              timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def default_sink() -> str:
    return _pactl("get-default-sink")


def hardware_sinks() -> list[str]:
    """Every real output, ours excluded."""
    out = []
    for line in _pactl("list", "short", "sinks").splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[1] != "semibreve":
            out.append(parts[1])
    return out


class Supervisor(QObject):
    """Keeps exactly one daemon alive for as long as the app is open."""

    failed = Signal(str)

    def __init__(self, daemon: DaemonService, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._daemon = daemon
        self._proc: subprocess.Popen | None = None
        self._ours = False          # did we start the one that is running?
        self._restarts = 0
        self._spawned_at = 0.0
        self._gone_since: float | None = None
        self._stopping = False

        self._watch = QTimer(self)
        self._watch.setInterval(1000)
        self._watch.timeout.connect(self._check)

    # -- lifecycle --------------------------------------------------------

    def start(self) -> None:
        """Make sure a daemon exists. Adopts one that is already running."""
        if self._daemon.available:
            log.info("adopting a daemon that was already running")
            self._ours = False
        else:
            self._spawn()
        self._watch.start()

    def _spawn(self) -> bool:
        if not DAEMON.exists():
            self.failed.emit(f"{DAEMON} is missing — run build.sh")
            return False

        # Hand it a real device explicitly. Left to follow the default it would
        # end up pointing at itself the moment Semibreve becomes the default,
        # and the graph really does build that feedback loop.
        target = default_sink()
        if not target or target == "semibreve":
            devices = hardware_sinks()
            target = devices[0] if devices else ""

        argv = [str(DAEMON)]
        if target:
            argv += ["--output", target]

        try:
            logfile = open(LOG_PATH, "ab", buffering=0)
        except OSError:
            logfile = subprocess.DEVNULL
        try:
            # start_new_session so the daemon is not in the UI's process group
            # and does not take a terminal Ctrl-C with it.
            self._proc = subprocess.Popen(
                argv, stdout=logfile, stderr=logfile,
                start_new_session=True, cwd=str(PROJECT_ROOT),
            )
        except OSError as exc:
            self.failed.emit(f"could not start the daemon: {exc}")
            return False

        self._ours = True
        self._spawned_at = time.monotonic()
        log.info("started daemon pid %s -> %s", self._proc.pid, target or "default sink")
        return True

    def stop(self) -> None:
        """Stop the daemon, but only if we were the ones who started it."""
        self._stopping = True
        self._watch.stop()
        if not self._ours:
            log.info("leaving the daemon running — it was not ours to stop")
            return

        # Ask over D-Bus first: it unwinds the streams and gives the sink up
        # tidily, where a signal would just cut it off.
        if self._daemon.available:
            self._daemon.quit_daemon()

        proc = self._proc
        if proc is None:
            return
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            log.warning("daemon did not exit on request; terminating")
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                proc.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except OSError:
                    pass
        self._proc = None

    # -- supervision ------------------------------------------------------

    def _check(self) -> None:
        if self._stopping:
            return
        if not self._ours:
            self._check_adopted()
            return
        proc = self._proc
        if proc is None or proc.poll() is None:
            return      # still running

        code = proc.returncode
        self._proc = None

        # Ours stopped but the name is held: another daemon is serving (a
        # second window got there first, say), and ours exited because it
        # would not start a second sink. Adopt that one rather than retry.
        other = self._daemon.owner_pid()
        if other and other != proc.pid:
            log.info("daemon pid %s is serving; adopting it", other)
            self._ours = False
            return
        if time.monotonic() - self._spawned_at >= STABLE_SECONDS:
            self._restarts = 0
        self._restarts += 1
        if self._restarts > MAX_RESTARTS:
            self._watch.stop()
            self.failed.emit(
                f"the daemon keeps stopping (last exit {code}) — see {LOG_PATH}")
            return

        # Exit 1 is its deliberate "I lost the PipeWire graph"; anything else is
        # a surprise. Both want the same answer: put the sink back.
        log.warning("daemon exited (%s) — restarting", code)
        self._spawn()

    def _check_adopted(self) -> None:
        """A daemon someone else started: once it has been gone for
        ADOPT_GRACE_SECONDS, start our own."""
        if self._daemon.available:
            self._gone_since = None
            return
        now = time.monotonic()
        if self._gone_since is None:
            self._gone_since = now
        elif now - self._gone_since >= ADOPT_GRACE_SECONDS:
            log.info("the daemon this window adopted has gone; starting one")
            self._gone_since = None
            self._restarts = 0
            self._spawn()
