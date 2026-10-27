"""Semibreve — the UI for semibreved.

Entry point: faulthandler on, logging configured before any UI module is
imported, an excepthook that reaches the log, SIGTERM and SIGINT routed
through Quit, and ``os._exit`` after ``app.exec()`` so a stuck Qt teardown
cannot hang the process.
"""

from __future__ import annotations

import faulthandler
import logging
import os
import signal
import socket
import sys
import traceback

from PySide6.QtCore import QSocketNotifier
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QApplication

log = logging.getLogger("semibreve")

# Held on the session bus while the window runs; see _claim_instance.
APP_BUS_NAME = "org.semibreve.App"


def _excepthook(et, ev, tb) -> None:
    traceback.print_exception(et, ev, tb)
    log.error("uncaught exception", exc_info=(et, ev, tb))


def _quit_on_signals(window) -> tuple:
    """End the app through Quit on SIGTERM and SIGINT.

    bin/semibreve --stop sends SIGTERM, and Python's default is to die on the
    spot: nothing saved since the window was last closed, and a daemon this
    window started left for the script to kill. Python runs a handler only
    between bytecodes, which Qt's loop never gives it, so the signal comes in
    through a socket the loop watches. Returns what has to stay alive.
    """
    rsock, wsock = socket.socketpair()
    rsock.setblocking(False)
    wsock.setblocking(False)
    signal.set_wakeup_fd(wsock.fileno())
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda _sig, _frame: window.quit_app())

    def drain() -> None:
        try:
            rsock.recv(64)
        except OSError:
            pass

    notifier = QSocketNotifier(rsock.fileno(), QSocketNotifier.Type.Read)
    notifier.activated.connect(drain)
    return rsock, wsock, notifier


def _claim_instance():
    """Hold APP_BUS_NAME for as long as this window runs.

    It is how bin/semibreve tells the window is already up. pgrep can't in a
    Flatpak, where every launch is a separate sandbox that sees only its own
    processes. Owning a name needs no D-Bus main loop, which this app doesn't
    run (see services/daemon.py), and a private connection keeps it apart
    from the daemon's polling. Returns the connection to keep alive, None if
    another window holds the name, or False if there is no bus to ask.
    """
    import dbus
    try:
        bus = dbus.SessionBus(private=True)
        reply = bus.request_name(APP_BUS_NAME, dbus.bus.NAME_FLAG_DO_NOT_QUEUE)
    except dbus.exceptions.DBusException as exc:
        log.warning("could not claim %s: %s", APP_BUS_NAME, exc)
        return False
    if reply != dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER:
        bus.close()
        return None
    return bus


def main() -> None:
    faulthandler.enable()
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )
    sys.excepthook = _excepthook

    log.info("starting; argv=%r; cwd=%s", sys.argv, os.getcwd())

    app = QApplication(sys.argv)
    app.setApplicationName("Semibreve")
    app.setOrganizationName("semibreve")
    # Wayland takes the window's app_id from this, and Plasma matches it
    # against semibreve.desktop to give the task manager a name and an icon.
    # Without the .desktop installed the entry is nameless and iconless, which
    # is most of what "no taskbar entry" looks like in practice. In the
    # Flatpak the entry and icon are named for the app ID instead.
    app_id = os.environ.get("FLATPAK_ID", "semibreve")
    app.setDesktopFileName(app_id)

    # By theme name, so KWin draws the icon itself. A pixmap reaches KWin as
    # pixel buffers whose alpha it misreads, which turns every anti-aliased
    # edge black or red in the title bar and task manager. The painted glyph
    # is only the fallback, for when semibreve.svg has not been installed.
    from semibreve.ui.tray import app_icon
    app.setWindowIcon(QIcon.fromTheme(app_id, app_icon()))

    # One window at a time: a second would start its own tray icon and
    # fight the first over the daemon.
    instance = _claim_instance()
    if instance is None:
        log.info("another window holds %s; exiting", APP_BUS_NAME)
        os._exit(0)

    # Closing the window hides it to the tray, so Qt must not take the last
    # window going away as a reason to exit. Quit is the tray menu's job.
    app.setQuitOnLastWindowClosed(False)

    # Imported here so logging is configured before any UI module loads.
    from semibreve.ui.main_window import MainWindow

    window = MainWindow()
    window.show()
    wakeup = _quit_on_signals(window)

    ret = app.exec()
    del wakeup, instance
    os._exit(ret)


if __name__ == "__main__":
    main()
