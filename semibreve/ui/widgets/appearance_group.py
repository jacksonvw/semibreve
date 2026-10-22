"""The 'Window' QGroupBox for the Preferences dialog.

Four controls:
- Background color picker (60×24 button)
- Opacity slider (0..100%, 100% = solid)
- Blur behind checkbox (KDE)
- Tint strength slider (0..100%)
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QWidget,
)

from semibreve.core.config import AppearanceConfig


class AppearanceGroup(QGroupBox):
    """Editor for an AppearanceConfig.

    Mutates the passed-in `AppearanceConfig` on `apply_to_config()`.
    Caller is responsible for save/load and broadcast.
    """

    def __init__(self, config: AppearanceConfig, parent: QWidget | None = None) -> None:
        super().__init__("Window", parent)
        self._config = config
        self._build_ui()

    def _build_ui(self) -> None:
        form = QFormLayout(self)

        # Background color picker.
        self._bg_color = QColor(*self._config.bg_color)
        self._color_btn = QPushButton()
        self._color_btn.setFixedSize(60, 24)
        self._update_color_btn()
        self._color_btn.clicked.connect(self._pick_bg_color)
        form.addRow("Background color:", self._color_btn)

        # Opacity slider.
        self._opacity_slider = QSlider(Qt.Horizontal)
        self._opacity_slider.setRange(0, 100)
        self._opacity_slider.setValue(int(self._config.window_opacity * 100))
        self._opacity_label = QLabel(f"{self._opacity_slider.value()}%")
        self._opacity_slider.valueChanged.connect(
            lambda v: self._opacity_label.setText(f"{v}%")
        )
        opacity_row = QHBoxLayout()
        opacity_row.addWidget(self._opacity_slider)
        opacity_row.addWidget(self._opacity_label)
        form.addRow("Opacity:", opacity_row)

        # Blur behind.
        self._blur_behind_cb = QCheckBox("Enable blur behind window (KDE)")
        self._blur_behind_cb.setChecked(self._config.blur_behind)
        form.addRow(self._blur_behind_cb)

        # Tint strength.
        self._tint_slider = QSlider(Qt.Horizontal)
        self._tint_slider.setRange(0, 100)
        self._tint_slider.setValue(self._config.bg_tint_strength)
        self._tint_label = QLabel(f"{self._tint_slider.value()}%")
        self._tint_slider.valueChanged.connect(
            lambda v: self._tint_label.setText(f"{v}%")
        )
        tint_row = QHBoxLayout()
        tint_row.addWidget(self._tint_slider)
        tint_row.addWidget(self._tint_label)
        form.addRow("Tint strength:", tint_row)

    def _update_color_btn(self) -> None:
        self._color_btn.setStyleSheet(
            f"background-color: {self._bg_color.name()}; border: 1px solid #666;"
        )

    def _pick_bg_color(self) -> None:
        color = QColorDialog.getColor(self._bg_color, self, "Background Color")
        if color.isValid():
            self._bg_color = color
            self._update_color_btn()

    def apply_to_config(self) -> None:
        """Write current UI state into the wrapped AppearanceConfig."""
        self._config.window_opacity = self._opacity_slider.value() / 100.0
        self._config.blur_behind = self._blur_behind_cb.isChecked()
        self._config.bg_color = [
            self._bg_color.red(), self._bg_color.green(), self._bg_color.blue()
        ]
        self._config.bg_tint_strength = self._tint_slider.value()
