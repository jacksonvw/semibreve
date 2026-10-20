"""Semibreve main window — power, presets, the five effects, the EQ.

Appearance: no ``setWindowOpacity``. ``WA_TranslucentBackground`` with a
``paintEvent`` fill whose alpha is ``window_opacity * bg_tint_strength``, and
KWin blur through ``semibreve.core.blur``.

The daemon owns the state and this window is a view of it. Every control sends
its change and then waits to be told what happened — it does not assume the
write landed. That matters because ``busctl`` or a second copy of this window
can move the same values, and because a preset load is applied on a timer
inside the daemon rather than synchronously.
"""

from __future__ import annotations

import logging

from PySide6.QtCore import QSettings, Qt, QTimer
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from semibreve.core.blur import set_blur_behind
from semibreve.core.config import load_config, save_config
from semibreve.services.daemon import EFFECTS, DaemonService
from semibreve.services.supervisor import Supervisor
from semibreve.ui.widgets.effect_slider import EffectSlider
from semibreve.ui.widgets.eq_view import EqView
from semibreve.ui.tray import Tray, app_icon
from semibreve.ui.widgets.power_button import PowerButton

log = logging.getLogger(__name__)

# The transparent stylesheet, so chrome stays legible against an arbitrary
# desktop behind the blur.
_TRANSPARENT_STYLESHEET = """
    QMainWindow, QWidget#central
    { background: transparent; }

    QLabel
    { background: transparent; color: rgba(255, 255, 255, 200); }
    QLabel#heading
    { color: rgba(255, 255, 255, 120); }
    QLabel#banner
    { color: rgba(255, 190, 120, 230); }

    QPushButton
    { background: rgba(255, 255, 255, 12); color: rgba(255, 255, 255, 200);
      border: 1px solid rgba(255, 255, 255, 20); padding: 4px 8px;
      border-radius: 3px; }
    QPushButton:hover   { background: rgba(255, 255, 255, 25); }
    QPushButton:pressed { background: rgba(255, 255, 255, 8); }

    QComboBox
    { background: rgba(255, 255, 255, 12); color: rgba(255, 255, 255, 210);
      border: 1px solid rgba(255, 255, 255, 20); padding: 3px 8px;
      border-radius: 3px; }
    QComboBox QAbstractItemView
    { background: rgba(40, 40, 40, 240); color: rgba(255, 255, 255, 220);
      selection-background-color: rgba(255, 255, 255, 30); }

    QSlider::groove:horizontal
    { background: rgba(255, 255, 255, 25); height: 4px; border-radius: 2px; }
    QSlider::handle:horizontal
    { background: rgba(255, 255, 255, 180); width: 12px; height: 12px;
      margin: -4px 0; border-radius: 6px; }
    QSlider::sub-page:horizontal
    { background: palette(highlight); border-radius: 2px; }

    QSlider::groove:vertical
    { background: rgba(255, 255, 255, 25); width: 4px; border-radius: 2px; }
    QSlider::handle:vertical
    { background: rgba(255, 255, 255, 180); width: 12px; height: 10px;
      margin: 0 -4px; border-radius: 5px; }

    QScrollArea { background: transparent; border: none; }
    QFrame#rule { background: rgba(255, 255, 255, 22); }
"""


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Semibreve")
        self.setWindowIcon(app_icon())

        self._config = load_config()
        self._bg_alpha = 255
        self._bg_color = [0, 0, 0]
        self._bg_tint_alpha = 178
        self._startup_preset_done = False
        # Set only by the tray's Quit. Everything else that looks like closing
        # is a hide.
        self._quitting = False
        self._sound_captured = False

        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)

        self._daemon = DaemonService(self)

        # The app owns the daemon: started here, restarted if it dies, stopped
        # on quit. Without this, quitting left the sink behind as the user's
        # default output with nothing driving it.
        self._supervisor = Supervisor(self._daemon, self)
        self._supervisor.failed.connect(self._on_supervisor_failed)

        central = QWidget()
        central.setObjectName("central")
        outer = QVBoxLayout(central)
        outer.setContentsMargins(16, 14, 16, 14)
        outer.setSpacing(12)

        # -- banner, shown only when there is no daemon ------------------
        self._banner = QLabel("")
        self._banner.setObjectName("banner")
        self._banner.setWordWrap(True)
        self._banner.hide()
        outer.addWidget(self._banner)

        # -- top row: power, preset --------------------------------------
        top = QHBoxLayout()
        top.setSpacing(16)

        self._power = PowerButton()
        self._power.toggled.connect(self._on_power_toggled)
        top.addWidget(self._power, 0, Qt.AlignmentFlag.AlignTop)

        preset_col = QVBoxLayout()
        preset_col.setSpacing(4)
        heading = QLabel("Preset")
        heading.setObjectName("heading")
        preset_col.addWidget(heading)

        self._preset = QComboBox()
        self._preset.setMinimumWidth(220)
        self._preset.activated.connect(self._on_preset_activated)
        preset_col.addWidget(self._preset)

        self._preset_note = QLabel("")
        self._preset_note.setObjectName("heading")
        preset_col.addWidget(self._preset_note)

        # Where the processed audio goes. FxSound has this and it is the
        # difference between "an effects box" and "an effects box I can use
        # with headphones and a soundbar".
        out_heading = QLabel("Output device")
        out_heading.setObjectName("heading")
        preset_col.addWidget(out_heading)

        self._output = QComboBox()
        self._output.setMinimumWidth(220)
        self._output.activated.connect(self._on_output_activated)
        preset_col.addWidget(self._output)

        preset_col.addStretch(1)
        top.addLayout(preset_col, 1)

        outer.addLayout(top)
        outer.addWidget(self._rule())

        # -- the five effects ---------------------------------------------
        effects_heading = QLabel("Effects")
        effects_heading.setObjectName("heading")
        outer.addWidget(effects_heading)

        self._sliders: dict[str, EffectSlider] = {}
        for prop, label in EFFECTS:
            slider = EffectSlider(label)
            slider.value_changed.connect(
                lambda v, p=prop: self._daemon.set_effect(p, v)
            )
            outer.addWidget(slider)
            self._sliders[prop] = slider

        outer.addWidget(self._rule())

        # -- the EQ, collapsed by default ----------------------------------
        eq_row = QHBoxLayout()
        self._eq_toggle = QPushButton()
        self._eq_toggle.clicked.connect(self._on_eq_toggle)
        eq_row.addWidget(self._eq_toggle)
        eq_row.addStretch(1)
        self._prefs_button = QPushButton("Preferences…")
        self._prefs_button.clicked.connect(self._on_preferences)
        eq_row.addWidget(self._prefs_button)
        outer.addLayout(eq_row)

        self._eq = EqView()
        self._eq.band_changed.connect(self._daemon.set_eq_band)
        outer.addWidget(self._eq)

        outer.addStretch(1)
        self.setCentralWidget(central)

        self._eq.setVisible(self._config.show_eq)
        self._update_eq_toggle_text()

        # -- wiring ----------------------------------------------------------
        self._daemon.availability_changed.connect(self._on_availability)
        self._daemon.effect_changed.connect(self._on_effect_changed)
        self._daemon.bypass_changed.connect(self._on_bypass_changed)
        self._daemon.preset_changed.connect(lambda *_: self._refresh_preset())
        self._daemon.eq_changed.connect(self._refresh_eq)
        self._daemon.output_changed.connect(self._refresh_outputs)

        # The tray is how you tell it is running and how you get the window
        # back when it is buried. It drives the same DaemonService, so it can
        # never disagree with the window.
        self._tray = Tray(self._daemon, self)
        self._tray.show_requested.connect(self._on_tray_show)
        self._tray.preferences_requested.connect(self._on_tray_preferences)
        self._tray.quit_requested.connect(self._on_tray_quit)

        self._restore_geometry()
        self._apply_appearance()
        self._supervisor.start()
        self._on_availability(self._daemon.available)

    # -- small helpers ----------------------------------------------------

    def _rule(self) -> QFrame:
        line = QFrame()
        line.setObjectName("rule")
        line.setFrameShape(QFrame.Shape.HLine)
        line.setFixedHeight(1)
        return line

    def _update_eq_toggle_text(self) -> None:
        self._eq_toggle.setText(
            "Hide equaliser" if self._eq.isVisible() else "Show equaliser"
        )

    def _on_preferences(self) -> None:
        from semibreve.ui.dialogs.preferences import PreferencesDialog

        dlg = PreferencesDialog(self)
        dlg.settings_changed.connect(self._reload_appearance)
        dlg.exec()

    def _reload_appearance(self) -> None:
        """Pick up appearance changes without disturbing audio state."""
        self._config.appearance = load_config().appearance
        self._apply_appearance()

    def _on_eq_toggle(self) -> None:
        show = not self._eq.isVisible()
        self._eq.setVisible(show)
        self._update_eq_toggle_text()
        self._config.show_eq = show
        if show:
            self._refresh_eq()

    # -- daemon state -----------------------------------------------------

    def _on_availability(self, available: bool) -> None:
        for w in (self._power, self._preset, self._output, self._eq,
                  self._eq_toggle, *self._sliders.values()):
            w.setEnabled(available)

        if available:
            self._banner.hide()
            self._reload_presets()
            self._refresh_all()
            self._restore_sound()
        else:
            self._banner.setText("Starting the audio engine…")
            self._banner.show()
            self._preset_note.setText("")

    def _refresh_all(self) -> None:
        for prop, slider in self._sliders.items():
            slider.set_value(self._daemon.effect(prop))
        self._power.set_on(not self._daemon.bypass())
        self._refresh_preset()
        self._refresh_outputs()
        self._refresh_eq()

    def _refresh_outputs(self) -> None:
        """Rebuild the device list and select the one in use.

        Rebuilt rather than patched because devices come and go — plugging in
        a DAC should just make it appear. blockSignals so repopulating does not
        read as the user choosing something.
        """
        devices = self._daemon.output_devices()
        current = self._daemon.output_device()

        self._output.blockSignals(True)
        self._output.clear()
        for name, description in devices:
            self._output.addItem(description or name, name)
        index = self._output.findData(current)
        self._output.setCurrentIndex(index if index >= 0 else -1)
        self._output.blockSignals(False)

    def _on_output_activated(self, index: int) -> None:
        name = self._output.itemData(index)
        if name:
            self._daemon.set_output_device(str(name))

    def _refresh_preset(self) -> None:
        pid = self._daemon.preset_id()
        name = self._daemon.preset_name()
        index = self._preset.findData(pid)
        if index >= 0:
            self._preset.setCurrentIndex(index)
        elif self._preset.count():
            # The daemon is on something not in the stock list — a .fac passed
            # on its command line, or nothing at all. Say so rather than
            # showing an unrelated entry as if it were selected.
            self._preset.setCurrentIndex(-1)
        self._preset_note.setText(name or "")

    def _refresh_eq(self) -> None:
        if self._eq.isVisible():
            self._eq.set_bands(self._daemon.eq_bands())

    def _reload_presets(self) -> None:
        presets = self._daemon.presets()
        self._preset.blockSignals(True)
        self._preset.clear()
        for pid, name in presets:
            # Several factory presets are numbered, so "6" alone is useless.
            # Show the name, and the id after it when they differ.
            label = name if name == pid else f"{name}  ({pid})"
            self._preset.addItem(label, pid)
        self._preset.blockSignals(False)

    def _restore_sound(self) -> None:
        """Put back what the user last had, once, on first connection.

        Order matters. The preset moves every effect and every EQ band, so it
        goes first and the individual values are laid over it — otherwise a
        preset load a moment later would wipe them. The preset is applied under
        a fade inside the daemon, hence the delay before the rest.
        """
        if self._startup_preset_done:
            return
        self._startup_preset_done = True
        sound = self._config.sound

        if sound.output_device:
            self._daemon.set_output_device(sound.output_device)

        if sound.preset:
            err = self._daemon.load_preset(sound.preset)
            if err:
                log.warning("saved preset %r: %s", sound.preset, err)
                QTimer.singleShot(0, self._apply_saved_values)
                return
            # Wait out the daemon's apply-and-fade before overlaying values.
            QTimer.singleShot(250, self._apply_saved_values)
        else:
            self._apply_saved_values()

    def _apply_saved_values(self) -> None:
        sound = self._config.sound
        for prop, value in sound.effects.items():
            if any(prop == key for key, _ in EFFECTS):
                self._daemon.set_effect(prop, value)
        for band, gain in enumerate(sound.eq):
            self._daemon.set_eq_band(band, gain)
        self._daemon.set_bypass(sound.bypass)
        self._refresh_all()

    def _capture_sound(self) -> None:
        """Record the current state so the next launch can restore it.

        Once only, and only while the daemon is actually there. Teardown calls
        the save path twice — the tray's Quit and then the window's close — and
        by the second the daemon has been stopped, so every read returns its
        default. That wrote `preset = ""` and every effect `0.0` over a good
        config, and because the next launch restored those zeros it stayed
        broken until the file was deleted.
        """
        if self._sound_captured or not self._daemon.available:
            return
        self._sound_captured = True
        sound = self._config.sound
        sound.preset = self._daemon.preset_id()
        sound.effects = {prop: self._daemon.effect(prop) for prop, _ in EFFECTS}
        sound.eq = [gain for _hz, gain in self._daemon.eq_bands()]
        sound.bypass = self._daemon.bypass()
        sound.output_device = self._daemon.output_device()

    def _on_supervisor_failed(self, message: str) -> None:
        log.error("supervisor: %s", message)
        self._banner.setText(message)
        self._banner.show()

    # -- control events ---------------------------------------------------

    def _on_power_toggled(self, on: bool) -> None:
        # The widget shows processing; the daemon speaks bypass.
        self._daemon.set_bypass(not on)

    def _on_preset_activated(self, index: int) -> None:
        pid = self._preset.itemData(index)
        if not pid:
            return
        err = self._daemon.load_preset(str(pid))
        if err:
            self._preset_note.setText(err)
            return
        # The daemon applies on a timer under a fade, and announces the result
        # when it lands. Until then, say something true.
        self._preset_note.setText("loading…")

    def _on_effect_changed(self, prop: str, value: float) -> None:
        slider = self._sliders.get(prop)
        if slider is not None:
            slider.set_value(value)

    def _on_bypass_changed(self, bypassed: bool) -> None:
        self._power.set_on(not bypassed)

    def _on_tray_show(self) -> None:
        """Raise the window, un-minimising it if it was iconified."""
        self.setWindowState(
            (self.windowState() & ~Qt.WindowState.WindowMinimized)
            | Qt.WindowState.WindowActive
        )
        self.show()
        self.raise_()
        self.activateWindow()

    def _on_tray_preferences(self) -> None:
        # Bring the window up first: a modal dialog with no visible parent is
        # a dialog the user cannot place.
        self._on_tray_show()
        self._on_preferences()

    def _on_tray_quit(self) -> None:
        """The only route that actually ends the app — and the daemon with it.

        Quitting has to take the sink away. Leaving it behind means the user
        has "exited" something that is still their default output device, with
        no window and no obvious way to be rid of it.

        Order matters: read the daemon's state *before* stopping it.
        """
        self._quitting = True
        self._save_state()
        self._supervisor.stop()
        self._tray.hide()
        QApplication.quit()

    # -- geometry ---------------------------------------------------------

    def _restore_geometry(self) -> None:
        settings = QSettings("semibreve", "semibreve")
        geo = settings.value("geometry")
        if geo is not None:
            self.restoreGeometry(geo)
        else:
            self.resize(560, 520)

    def _save_state(self) -> None:
        """Persist geometry and audio state.

        Appearance is re-read from disk and passed through untouched. Two
        things write this file — this window and the preferences dialog — and
        each must only own its own half, or whichever saves last silently
        reverts the other. The dialog does the mirror of this with sound state.
        """
        self._capture_sound()
        settings = QSettings("semibreve", "semibreve")
        settings.setValue("geometry", self.saveGeometry())
        try:
            on_disk = load_config()
            self._config.appearance = on_disk.appearance
            save_config(self._config)
        except OSError:
            log.exception("could not save config")

    def closeEvent(self, ev) -> None:
        """Close hides to the tray; only the tray's Quit really exits.

        The daemon keeps running either way — it is a separate process and the
        audio should not stop because a window was dismissed. Hiding rather
        than quitting means the window comes back instantly and the tray icon
        stays as the answer to "is this on?".
        """
        self._save_state()
        if self._quitting:
            self._tray.hide()
            super().closeEvent(ev)
            return

        ev.ignore()
        self.hide()

    # -- appearance -------------------------------------------------------

    def _apply_appearance(self) -> None:
        appearance = self._config.appearance
        self._bg_alpha = int(appearance.window_opacity * 255)
        self._bg_color = list(appearance.bg_color)
        self._bg_tint_alpha = int(appearance.bg_tint_strength * 255 / 100)

        if appearance.window_opacity < 1.0:
            self.setStyleSheet(_TRANSPARENT_STYLESHEET)
        else:
            self.setStyleSheet("")
            self._bg_alpha = 255

        self._apply_blur()
        self.update()

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        r, g, b = self._bg_color
        if self._bg_alpha < 255:
            tint_alpha = int(self._bg_tint_alpha * self._bg_alpha / 255)
            painter.fillRect(self.rect(), QColor(r, g, b, tint_alpha))
        else:
            painter.fillRect(self.rect(), QColor(r, g, b))
        painter.end()

    def _apply_blur(self) -> None:
        appearance = self._config.appearance
        want = appearance.blur_behind and appearance.window_opacity < 1.0
        set_blur_behind(self, want)

    def showEvent(self, ev) -> None:
        # windowHandle() is None until now, so this is the first point at
        # which blur can be applied. Qt may recreate the native window later,
        # which lands here again.
        super().showEvent(ev)
        self._apply_blur()
