"""System tray icon — the thing that tells you it is running.

FxSound lives in the tray and Semibreve should too. Without it there is no way
to tell the daemon is up, no way to toggle processing without finding the
window, and — if the window is behind something — no obvious way to get back to
it. That was the complaint that produced this file.

The icon reflects processing state, so a glance at the panel answers "is it
on?". Everything the menu does goes through the same DaemonService as the
window, so the two can never disagree.
"""

from __future__ import annotations

import logging
from PySide6.QtCore import QObject, Signal
from PySide6.QtGui import QAction, QIcon
from PySide6.QtWidgets import QMenu, QSystemTrayIcon

from semibreve.services.daemon import DaemonService
from semibreve.ui.icon import accent_color, idle_color, off_color, icon as glyph_icon

log = logging.getLogger(__name__)


def app_icon() -> QIcon:
    """The window and task-manager icon: the glyph in its "on" colour.

    Not the installed theme icon, and not the SVG. Both render black through
    Qt, which disappears against a dark panel — see semibreve/ui/icon.py.
    """
    return glyph_icon(accent_color())


class Tray(QObject):
    """Tray icon and menu. Emits when the user asks to see the window."""

    show_requested = Signal()
    preferences_requested = Signal()
    quit_requested = Signal()

    def __init__(self, daemon: DaemonService, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._daemon = daemon

        self._icon = QSystemTrayIcon(glyph_icon(idle_color()), self)
        self._icon.setToolTip("Semibreve")

        menu = QMenu()

        self._show_action = QAction("Show Semibreve", menu)
        self._show_action.triggered.connect(self.show_requested)
        menu.addAction(self._show_action)

        menu.addSeparator()

        # Checkable rather than a label that flips, so the state is readable
        # without having to remember what the last click did.
        self._power_action = QAction("Processing", menu)
        self._power_action.setCheckable(True)
        self._power_action.toggled.connect(self._on_power)
        menu.addAction(self._power_action)

        self._status_action = QAction("", menu)
        self._status_action.setEnabled(False)
        menu.addAction(self._status_action)

        menu.addSeparator()

        prefs_action = QAction("Preferences…", menu)
        prefs_action.triggered.connect(self.preferences_requested)
        menu.addAction(prefs_action)

        quit_action = QAction("Quit", menu)
        quit_action.triggered.connect(self.quit_requested)
        menu.addAction(quit_action)

        # Held on self: a QMenu owned only by a local goes away with it and the
        # tray icon is left with a dangling context menu.
        self._menu = menu
        self._icon.setContextMenu(menu)
        self._icon.activated.connect(self._on_activated)

        daemon.availability_changed.connect(self._refresh)
        daemon.bypass_changed.connect(lambda _b: self._refresh())
        daemon.preset_changed.connect(lambda *_: self._refresh())

        self._refresh()
        self._icon.show()

    def _on_activated(self, reason) -> None:
        if reason in (QSystemTrayIcon.ActivationReason.Trigger,
                      QSystemTrayIcon.ActivationReason.DoubleClick):
            self.show_requested.emit()

    def _on_power(self, on: bool) -> None:
        # Guarded: _refresh sets the checkmark from daemon state, which would
        # otherwise bounce straight back as a command.
        if getattr(self, "_echo", False):
            return
        self._daemon.set_bypass(not on)

    def _refresh(self) -> None:
        available = self._daemon.available
        processing = available and not self._daemon.bypass()

        self._echo = True
        try:
            self._power_action.setChecked(processing)
        finally:
            self._echo = False
        self._power_action.setEnabled(available)

        # The icon is the state: green processing, grey bypassed, dim when
        # there is no daemon at all.
        self._icon.setIcon(glyph_icon(
            accent_color() if processing else (off_color() if available else idle_color())))

        if not available:
            self._status_action.setText("daemon not running")
            self._icon.setToolTip("Semibreve — daemon not running")
            return

        name = self._daemon.preset_name() or "Custom"
        state = "processing" if processing else "bypassed"
        self._status_action.setText(f"{name} · {state}")
        self._icon.setToolTip(f"Semibreve — {name}, {state}")

    def hide(self) -> None:
        self._icon.hide()
