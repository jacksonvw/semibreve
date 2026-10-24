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
    # is most of what "no taskbar entry" looks like in practice.
    app.setDesktopFileName("semibreve")

    # By theme name, so KWin draws the icon itself. A pixmap reaches KWin as
    # pixel buffers whose alpha it misreads, which turns every anti-aliased
    # edge black or red in the title bar and task manager. The painted glyph
    # is only the fallback, for when semibreve.svg has not been installed.
    from semibreve.ui.tray import app_icon
    app.setWindowIcon(QIcon.fromTheme("semibreve", app_icon()))

    # Closing the window hides it to the tray, so Qt must not take the last
    # window going away as a reason to exit. Quit is the tray menu's job.
    app.setQuitOnLastWindowClosed(False)

    # Imported here so logging is configured before any UI module loads.
    from semibreve.ui.main_window import MainWindow

    window = MainWindow()
    window.show()
    wakeup = _quit_on_signals(window)

    ret = app.exec()
    del wakeup
    os._exit(ret)


if __name__ == "__main__":
    main()
