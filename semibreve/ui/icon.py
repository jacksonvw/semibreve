"""The Semibreve glyph, drawn rather than loaded.

A whole note — which is what "semibreve" means.

Drawn in code because the tray icon has to be visible on *someone's* panel and
an SVG cannot be. Qt's SVG renderer resolves ``currentColor`` to black, so a
theme-following file renders as a black note that vanishes on Plasma's dark
panel; hard-coding white loses it on a light one. Painting it means the colour
can be chosen at draw time — the desktop's own accent when audio is being
processed, faded when it is not.
"""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QIcon, QPainter, QPainterPath, QPen, QPixmap
from PySide6.QtWidgets import QApplication


def accent_color() -> QColor:
    """The desktop's accent colour.

    Plasma publishes it through the palette, so following it means Semibreve
    matches whatever the user has chosen rather than imposing a colour of its
    own. Qt 6.6 added the Accent role; ``highlight`` is the same thing on
    Plasma and the fallback everywhere else.
    """
    app = QApplication.instance()
    if app is not None:
        palette = app.palette()
        if hasattr(palette, "accent"):
            colour = palette.accent().color()
            if colour.isValid():
                return colour
        colour = palette.highlight().color()
        if colour.isValid():
            return colour
    return QColor(48, 140, 198)


def off_color() -> QColor:
    """Bypassed: the accent with the colour taken out of it.

    Desaturating the accent rather than following windowText, which is black on
    a light theme and white on a dark one — and a black tray icon on a dark
    panel is exactly the invisible-icon problem this module exists to avoid.
    Desaturating keeps the same lightness as the accent, so it stays legible
    wherever the accent was.
    """
    colour = accent_color().toHsv()
    colour.setHsv(colour.hue(), int(colour.saturation() * 0.12),
                  colour.value(), 255)
    return colour.toRgb()


def idle_color() -> QColor:
    """No daemon at all: the bypassed colour, half faded."""
    colour = off_color()
    colour.setAlpha(110)
    return colour


def render(size: int, colour: QColor) -> QPixmap:
    """A whole note at ``size`` px, in ``colour``, on transparency."""
    pm = QPixmap(size, size)
    pm.fill(Qt.GlobalColor.transparent)

    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.translate(size / 2.0, size / 2.0)
    p.rotate(-20)

    # Proportions of a real whole note: a wide ellipse with an ellipse of hole.
    rx, ry = size * 0.34, size * 0.22
    stroke = max(1.5, size * 0.10)

    p.setPen(QPen(colour, stroke))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawEllipse(QRectF(-rx, -ry, rx * 2, ry * 2))

    # The counter, filled, giving the note its slanted eye.
    path = QPainterPath()
    path.addEllipse(QRectF(-size * 0.15, -size * 0.09, size * 0.30, size * 0.18))
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(colour)
    p.drawPath(path)
    p.end()
    return pm


def icon(colour: QColor) -> QIcon:
    """A multi-resolution icon, so the tray and the task manager both get a
    crisp one rather than a scaled 64px."""
    out = QIcon()
    for s in (16, 22, 24, 32, 48, 64, 128):
        out.addPixmap(render(s, colour))
    return out
