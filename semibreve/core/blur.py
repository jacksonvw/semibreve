"""KWin blur-behind for translucent windows — Wayland *and* X11.

Why ctypes
----------
KWin's blur is exposed through KWindowSystem's ``KWindowEffects``, which
picks its own backend at runtime: the ``org_kde_kwin_blur`` Wayland
protocol under a Wayland session, and ``_KDE_NET_WM_BLUR_BEHIND_REGION``
under X11. That makes it the one call that works on both.

There is no Python binding for KF6, and PySide6 ships no QtWayland module,
so we dlopen the library and call the single C++ entry point we need. The
alternatives were a compiled helper extension, or pywayland plus the Plasma
protocol XML (``plasma-wayland-protocols``, not installed) — both a lot of
machinery for one function.

This replaces an earlier approach that shelled out to ``xprop``. That was
X11-only and silently no-opped under Wayland, which is why ``blur_behind``
appeared to do nothing.

Failure is always soft: if KWindowSystem is missing, the symbol has moved,
or the compositor is not KWin, blur is skipped and the window still renders
correctly — just without blur.
"""

from __future__ import annotations

import ctypes
import logging

import shiboken6
from PySide6.QtGui import QRegion

log = logging.getLogger(__name__)

_LIB = "libKF6WindowSystem.so.6"

# KWindowEffects::enableBlurBehind(QWindow *, bool, QRegion const &)
# Itanium C++ ABI mangling. Stable as long as that signature is; if KF ever
# changes it, _resolve() fails cleanly and blur is simply skipped.
_SYMBOL = "_ZN14KWindowEffects16enableBlurBehindEP7QWindowbRK7QRegion"

_resolved = False
_enable_blur_behind = None


def _resolve():
    """dlopen KWindowSystem and bind the symbol. Cached, including failure."""
    global _resolved, _enable_blur_behind
    if _resolved:
        return _enable_blur_behind
    _resolved = True
    try:
        fn = getattr(ctypes.CDLL(_LIB), _SYMBOL)
    except (OSError, AttributeError) as exc:
        log.info("blur-behind unavailable (%s): %s", _LIB, exc)
        return None
    # QWindow*, bool, const QRegion& — a const-ref is a pointer in the ABI.
    fn.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_void_p]
    fn.restype = None
    _enable_blur_behind = fn
    return fn


def is_available() -> bool:
    """True if KWindowSystem could be loaded and the symbol bound."""
    return _resolve() is not None


def set_blur_behind(widget, enable: bool = True) -> bool:
    """Enable or disable KWin blur behind ``widget``.

    Returns True if the request reached KWin. Returns False — without
    raising — when blur is unavailable, or when the widget has no native
    window yet. ``windowHandle()`` is None until the widget is first shown,
    so callers that run during ``__init__`` must re-apply from ``showEvent``.
    """
    fn = _resolve()
    if fn is None:
        return False

    handle = widget.windowHandle()
    if handle is None:
        return False

    try:
        window_ptr = shiboken6.getCppPointer(handle)[0]
        # An empty QRegion means "the whole window" to KWindowEffects.
        # Held in a local so it outlives the (synchronous) call.
        region = QRegion()
        region_ptr = shiboken6.getCppPointer(region)[0]
        fn(ctypes.c_void_p(window_ptr), bool(enable), ctypes.c_void_p(region_ptr))
    except Exception:
        log.exception("blur-behind call failed")
        return False
    return True
