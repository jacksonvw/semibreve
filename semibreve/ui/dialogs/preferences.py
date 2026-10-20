"""Semibreve preferences — the Window groupbox.

Apply writes and takes effect without closing, OK does both, Cancel discards.

There is only the one groupbox. Everything else Semibreve can be told is audio
state, and that belongs on the main window next to the thing it changes rather
than buried in a dialog.
"""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import QDialog, QDialogButtonBox, QVBoxLayout, QWidget

from semibreve.core.config import AppConfig, load_config, save_config
from semibreve.ui.widgets.appearance_group import AppearanceGroup


class PreferencesDialog(QDialog):
    """Emits settings_changed after a successful Apply or OK."""

    settings_changed = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Preferences")
        self.setMinimumWidth(380)

        # Loaded fresh rather than shared with the window: Cancel has to leave
        # nothing behind, and the window's own copy carries live audio state
        # that this dialog has no business touching.
        self._config: AppConfig = load_config()
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        self._appearance = AppearanceGroup(self._config.appearance)
        layout.addWidget(self._appearance)
        layout.addStretch()

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
            | QDialogButtonBox.StandardButton.Apply
        )
        buttons.accepted.connect(self._on_ok)
        buttons.rejected.connect(self.reject)
        buttons.button(QDialogButtonBox.StandardButton.Apply).clicked.connect(self._on_apply)
        layout.addWidget(buttons)

    def _on_apply(self) -> None:
        self._appearance.apply_to_config()
        # Merge rather than overwrite: the window may have moved audio state
        # since this dialog opened, and saving our stale copy of it would undo
        # whatever the user just did to a slider.
        current = load_config()
        current.appearance = self._config.appearance
        save_config(current)
        self.settings_changed.emit()

    def _on_ok(self) -> None:
        self._on_apply()
        self.accept()
