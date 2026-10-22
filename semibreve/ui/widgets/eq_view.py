"""The graphic EQ — thirty-one vertical faders with their frequencies.

Thirty-one is FxSound's band count and not negotiable: it comes from
``DFXP_GRAPHIC_EQ_NUM_BANDS`` and the preset files carry a gain for each one.
They do not fit comfortably in a window, so this lives in a horizontal scroll
area and the window starts with it collapsed.

Frequencies are read from the daemon rather than assumed. The band centres are
the DSP's own (a fixed log-spaced table, not derived from anything we could
recompute correctly), and printing a rounded ISO series next to faders that
actually sit somewhere else would be a small lie that survives a long time.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QScrollArea,
    QSlider,
    QVBoxLayout,
    QWidget,
)

SCALE = 10           # slider units per dB
MIN_DB = -12.0
MAX_DB = 12.0


def _format_hz(hz: float) -> str:
    if hz >= 1000:
        text = f"{hz / 1000:.1f}"
        return (text[:-2] if text.endswith(".0") else text) + "k"
    return f"{hz:.0f}"


def _format_db(db: float) -> str:
    # Tenths, because the faders move in tenths and a few of them are audible.
    # Whole dB printed a -0.3 cut as "-0"; "z" keeps negative zero out for good.
    return f"{db:z.1f}"


class _Band(QWidget):
    """One fader, its gain readout and its frequency label."""

    value_changed = Signal(int, float)

    def __init__(self, index: int, hz: float, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._index = index
        self._echo = False

        self._readout = QLabel("0.0")
        self._readout.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        # Wide enough for any reading from the start. Sized to its text, a
        # readout that grew mid-drag widened its column and shoved every fader
        # to the right of it sideways.
        self._readout.setMinimumWidth(
            self._readout.fontMetrics().horizontalAdvance(_format_db(MIN_DB)))

        self._slider = QSlider(Qt.Orientation.Vertical)
        self._slider.setRange(int(MIN_DB * SCALE), int(MAX_DB * SCALE))
        self._slider.setValue(0)
        self._slider.setMinimumHeight(120)
        self._slider.valueChanged.connect(self._on_slider)

        self._hz = QLabel(_format_hz(hz))
        self._hz.setAlignment(Qt.AlignmentFlag.AlignHCenter)

        col = QVBoxLayout(self)
        col.setContentsMargins(0, 0, 0, 0)
        col.setSpacing(3)
        col.addWidget(self._readout)
        col.addWidget(self._slider, 1, Qt.AlignmentFlag.AlignHCenter)
        col.addWidget(self._hz)

    def set_gain(self, db: float) -> None:
        self._echo = True
        try:
            self._slider.setValue(int(round(max(MIN_DB, min(MAX_DB, db)) * SCALE)))
            self._readout.setText(_format_db(self._slider.value() / SCALE))
        finally:
            self._echo = False

    def _on_slider(self, raw: int) -> None:
        self._readout.setText(_format_db(raw / SCALE))
        if not self._echo:
            self.value_changed.emit(self._index, raw / SCALE)


class EqView(QScrollArea):
    """All thirty-one bands, scrolling horizontally."""

    band_changed = Signal(int, float)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._bands: list[_Band] = []

        self._inner = QWidget()
        self._row = QHBoxLayout(self._inner)
        self._row.setContentsMargins(4, 4, 4, 4)
        self._row.setSpacing(2)

        self.setWidget(self._inner)
        self.setWidgetResizable(True)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setMinimumHeight(190)

    def set_bands(self, bands: list[tuple[float, float]]) -> None:
        """Build or update the faders from (frequency, gain) pairs.

        Rebuilds only when the band count changes — which in practice means
        once, on first connection. Rebuilding on every update would destroy
        the widget under the user's cursor mid-drag.
        """
        if len(bands) != len(self._bands):
            while self._row.count():
                item = self._row.takeAt(0)
                w = item.widget()
                if w is not None:
                    w.deleteLater()
            self._bands = []
            for i, (hz, _gain) in enumerate(bands):
                band = _Band(i, hz, self._inner)
                band.value_changed.connect(self.band_changed)
                self._row.addWidget(band)
                self._bands.append(band)

        for band, (_hz, gain) in zip(self._bands, bands):
            band.set_gain(gain)
