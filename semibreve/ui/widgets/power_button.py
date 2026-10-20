"""The on/off switch.

This is the control that answers "is it doing anything?", so it is the one
thing on the window that should be readable from across the room. It is drawn
rather than styled because a QCheckBox with a stylesheet still reads as a
checkbox, and this wants to read as a power state.

It reflects *processing*, not bypass. The daemon's property is ``Bypass``
because that is what the DSP calls it — and note that upstream's own
``isPowerOn()`` returns the bypass button, inverted, which is exactly the
confusion this widget exists to keep out of the UI. On screen: lit means audio
is being processed.
"""

from __future__ import annotations

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtGui import QFont, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QWidget

from semibreve.ui.icon import accent_color, idle_color, off_color


class PowerButton(QWidget):
    """A large two-state toggle. Lit = processing, dim = bypassed."""

    toggled = Signal(bool)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._on = True
        self._hover = False
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMinimumSize(QSize(96, 96))
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def sizeHint(self) -> QSize:
        return QSize(108, 108)

    def is_on(self) -> bool:
        return self._on

    def set_on(self, on: bool) -> None:
        """Set the displayed state without emitting."""
        if self._on != bool(on):
            self._on = bool(on)
            self.update()

    def _toggle(self) -> None:
        if not self.isEnabled():
            return
        self._on = not self._on
        self.update()
        self.toggled.emit(self._on)

    def mouseReleaseEvent(self, ev) -> None:
        if ev.button() == Qt.MouseButton.LeftButton and self.rect().contains(ev.position().toPoint()):
            self._toggle()
        super().mouseReleaseEvent(ev)

    def keyPressEvent(self, ev) -> None:
        if ev.key() in (Qt.Key.Key_Space, Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self._toggle()
            return
        super().keyPressEvent(ev)

    def enterEvent(self, ev) -> None:
        self._hover = True
        self.update()
        super().enterEvent(ev)

    def leaveEvent(self, ev) -> None:
        self._hover = False
        self.update()
        super().leaveEvent(ev)

    def paintEvent(self, _ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)

        side = min(self.width(), self.height())
        margin = side * 0.12
        box = self.rect().adjusted(int(margin), int(margin), -int(margin), -int(margin))
        side = min(box.width(), box.height())

        # The desktop's accent when it is doing something, faded when not.
        if not self.isEnabled():
            colour = idle_color()
        elif self._on:
            colour = accent_color()
        else:
            colour = off_color()
        if self._hover and self.isEnabled():
            colour = colour.lighter(115)

        # Ring
        pen = QPen(colour, max(3.0, side * 0.075))
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        cx, cy = box.center().x(), box.center().y()
        r = side * 0.34
        # The standard power glyph: a ring with a gap at the top, and a stroke
        # through the gap. Angles are in sixteenths of a degree.
        p.drawArc(int(cx - r), int(cy - r), int(r * 2), int(r * 2), 65 * 16, 410 * 16)

        path = QPainterPath()
        path.moveTo(cx, cy - r * 1.12)
        path.lineTo(cx, cy - r * 0.18)
        p.drawPath(path)

        # Label underneath, so the state is unambiguous without relying on
        # colour alone.
        f: QFont = self.font()
        f.setPointSizeF(max(7.5, side * 0.13))
        f.setCapitalization(QFont.Capitalization.AllUppercase)
        f.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, 1.0)
        p.setFont(f)
        p.setPen(colour)
        text = "On" if self._on else "Bypassed"
        if not self.isEnabled():
            text = "No daemon"
        p.drawText(
            self.rect().adjusted(0, 0, 0, -int(margin * 0.2)),
            Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignBottom,
            text,
        )
        p.end()
