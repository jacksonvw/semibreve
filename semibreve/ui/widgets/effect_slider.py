"""One labelled effect control: name, slider, value.

The slider is integer-only, so the 0..10 the daemon speaks is carried as
0..1000 here and divided on the way out. A hundred steps per unit is finer
than the mouse can resolve and finer than the DSP's MIDI quantisation, so
nothing is lost by it and the arithmetic stays in integers where Qt wants it.

``setValue`` is guarded by ``_echo``. Without it, applying a value that arrived
from the daemon would emit ``valueChanged``, which would send it straight back
— and while a user is dragging, the round trip lands behind the drag and yanks
the handle backwards. Every widget here that both displays and edits daemon
state has the same guard for the same reason.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import QHBoxLayout, QLabel, QSlider, QWidget

SCALE = 100          # slider units per 1.0 of effect value
MAX_VALUE = 10.0


class EffectSlider(QWidget):
    """A single 0..10 effect knob."""

    value_changed = Signal(float)

    def __init__(self, label: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._echo = False

        self._name = QLabel(label)
        self._name.setMinimumWidth(110)

        self._slider = QSlider(Qt.Orientation.Horizontal)
        self._slider.setRange(0, int(MAX_VALUE * SCALE))
        self._slider.setSingleStep(SCALE // 10)
        self._slider.setPageStep(SCALE)
        self._slider.valueChanged.connect(self._on_slider)

        self._readout = QLabel("0.0")
        self._readout.setMinimumWidth(38)
        self._readout.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(10)
        row.addWidget(self._name)
        row.addWidget(self._slider, 1)
        row.addWidget(self._readout)

    def value(self) -> float:
        return self._slider.value() / SCALE

    def set_value(self, value: float) -> None:
        """Show a value without emitting. See the note at the top of the file."""
        self._echo = True
        try:
            self._slider.setValue(int(round(max(0.0, min(MAX_VALUE, value)) * SCALE)))
            self._readout.setText(f"{self._slider.value() / SCALE:.1f}")
        finally:
            self._echo = False

    def _on_slider(self, raw: int) -> None:
        self._readout.setText(f"{raw / SCALE:.1f}")
        if not self._echo:
            self.value_changed.emit(raw / SCALE)
