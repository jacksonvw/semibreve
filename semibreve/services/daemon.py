"""The daemon, as seen from the UI.

Wraps ``org.semibreve.Daemon`` on the session bus and presents it as an
ordinary QObject with signals. Nothing above this module knows that D-Bus
exists; nothing below it knows that Qt does.

Why dbus-python, and why polling
--------------------------------
QtDBus was the first choice — it ships inside PySide6 and is dispatched by Qt's
own loop — and it does not survive contact with this interface. PySide6 cannot
demarshal ``a(ss)`` or ``a(dd)``: ``ListPresets`` comes back as a
``QDBusArgument`` whose ``asVariant()`` yields further ``QDBusArgument``
objects rather than the strings inside, and ``EqBands`` reads as ``None`` outright. Its
``connect()`` also wants a slot by mangled C++ signature and rejected every
spelling of a registered ``@Slot(QDBusMessage)``.

Reshaping the daemon's interface into arrays of plain scalars would have made
PySide6 happy, and would have been the wrong trade: the interface should read
well from ``busctl`` and from any other client, not be bent around one
binding's gaps. dbus-python unpacks all of it natively.

That leaves signals. dbus-python needs a GLib main loop to deliver them, which
a Qt application does not run — so this polls instead and emits on change. The
whole state is nine property reads over a local socket four times a second,
nothing here is anywhere near an audio path, and a poll has one property a
subscription lacks: when it stops working it takes the availability check down
with it, visibly, instead of going quiet.

Availability is not assumed anywhere. The daemon can be stopped, restarted, or
never started at all, and the window has to stay sane throughout — so this
reports coming and going through ``availability_changed`` rather than raising.
"""

from __future__ import annotations

import logging

import dbus

from PySide6.QtCore import QObject, QTimer, Signal

log = logging.getLogger(__name__)

# Sentinel for "not polled yet", so a legitimate 0.0 or "" is not mistaken for
# an absent reading on the first pass.
_MISSING = object()

SERVICE = "org.semibreve.Daemon"
PATH = "/org/semibreve/Daemon"
IFACE = "org.semibreve.Daemon"
PROPS_IFACE = "org.freedesktop.DBus.Properties"

# The five effects, in DfxDsp::Effect order. The names are FxSound's and are
# baked into the preset files, so they are also the D-Bus property names — one
# spelling from the .fac on disk all the way to the label on screen.
EFFECTS: list[tuple[str, str]] = [
    ("Fidelity", "Fidelity"),
    ("Ambience", "Ambience"),
    ("Surround", "Surround"),
    ("DynamicBoost", "Dynamic Boost"),
    ("Bass", "Bass"),
]


class DaemonService(QObject):
    """Live view of semibreved's control surface."""

    availability_changed = Signal(bool)
    effect_changed = Signal(str, float)      # property name, 0..10
    bypass_changed = Signal(bool)
    preset_changed = Signal(str, str)        # id, display name
    eq_changed = Signal()
    output_changed = Signal()                # current device or the list moved

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        try:
            self._bus = dbus.SessionBus()
        except dbus.DBusException:
            log.exception("no session bus")
            self._bus = None
        self._props = None
        self._iface = None
        self._available = False

        # Last seen values, so the poll can emit only what actually moved.
        self._last: dict[str, object] = {}

        self._poll = QTimer(self)
        self._poll.setInterval(250)
        self._poll.timeout.connect(self._poll_once)
        self._poll.start()

        self._poll_once()

    # -- availability ---------------------------------------------------

    @property
    def available(self) -> bool:
        return self._available

    def _refresh_availability(self) -> bool:
        """Bind or drop the proxy. Returns True if the daemon is there."""
        if self._bus is None:
            return False
        try:
            present = SERVICE in self._bus.list_names()
        except dbus.DBusException:
            present = False

        if present and self._iface is None:
            try:
                obj = self._bus.get_object(SERVICE, PATH)
                self._iface = dbus.Interface(obj, IFACE)
                self._props = dbus.Interface(obj, PROPS_IFACE)
            except dbus.DBusException:
                log.exception("daemon present but could not be bound")
                self._iface = self._props = None
                return False
        elif not present:
            self._iface = self._props = None

        if present != self._available:
            self._available = present
            self._last.clear()
            log.info("daemon %s", "appeared" if present else "went away")
            self.availability_changed.emit(present)
        return present

    def _poll_once(self) -> None:
        """Read the daemon's state and emit whatever changed since last time.

        Deliberately quiet on the first pass after the daemon appears:
        ``availability_changed`` already tells the window to reload everything,
        so emitting a change for every property on top of that would just have
        it redraw twice.
        """
        was_available = self._available
        if not self._refresh_availability():
            return
        first = not was_available or not self._last

        for prop, _label in EFFECTS:
            self._diff(prop, self.effect(prop), first,
                       lambda p, v: self.effect_changed.emit(p, float(v)))
        self._diff("Bypass", self.bypass(), first,
                   lambda _p, v: self.bypass_changed.emit(bool(v)))
        self._diff("PresetId", self.preset_id(), first,
                   lambda _p, _v: self.preset_changed.emit(self.preset_id(),
                                                           self.preset_name()))
        self._diff("EqBands", self.eq_bands(), first,
                   lambda _p, _v: self.eq_changed.emit())
        self._diff("OutputDevice", self.output_device(), first,
                   lambda _p, _v: self.output_changed.emit())
        self._diff("OutputDevices", self.output_devices(), first,
                   lambda _p, _v: self.output_changed.emit())

    def _diff(self, key: str, value, quiet: bool, emit) -> None:
        if self._last.get(key, _MISSING) != value:
            self._last[key] = value
            if not quiet:
                emit(key, value)

    # -- reads ------------------------------------------------------------

    def _get(self, prop: str, default):
        """Read one property. Returns ``default`` if the daemon is not there.

        Every getter goes through here so that a daemon that vanishes mid-call
        degrades to a default instead of an exception in a paint handler.
        """
        if self._props is None:
            return default
        try:
            return self._props.Get(IFACE, prop)
        except dbus.DBusException:
            return default

    def effect(self, prop: str) -> float:
        try:
            return float(self._get(prop, 0.0))
        except (TypeError, ValueError):
            return 0.0

    def bypass(self) -> bool:
        return bool(self._get("Bypass", False))

    def preset_id(self) -> str:
        return str(self._get("PresetId", ""))

    def preset_name(self) -> str:
        return str(self._get("PresetName", ""))

    def eq_bands(self) -> list[tuple[float, float]]:
        """(centre frequency, gain dB) per band, or empty if unavailable."""
        raw = self._get("EqBands", None)
        if raw is None:
            return []
        out: list[tuple[float, float]] = []
        for entry in raw:
            try:
                out.append((float(entry[0]), float(entry[1])))
            except (TypeError, ValueError, IndexError):
                continue
        return out

    def presets(self) -> list[tuple[str, str]]:
        """(id, display name) for every stock preset."""
        if self._iface is None:
            return []
        try:
            return [(str(pid), str(name)) for pid, name in self._iface.ListPresets()]
        except (dbus.DBusException, TypeError, ValueError):
            log.exception("ListPresets failed")
            return []

    # -- writes -----------------------------------------------------------

    def _set(self, prop: str, value) -> None:
        if self._props is None:
            return
        try:
            self._props.Set(IFACE, prop, value)
        except dbus.DBusException:
            log.exception("setting %s failed", prop)

    def set_effect(self, prop: str, value: float) -> None:
        self._set(prop, dbus.Double(float(value)))

    def set_bypass(self, on: bool) -> None:
        self._set("Bypass", dbus.Boolean(bool(on)))

    def load_preset(self, name: str) -> str:
        """Ask for a preset. Returns "" on success, or the daemon's error."""
        if self._iface is None:
            return "not connected"
        try:
            self._iface.LoadPreset(str(name))
        except dbus.DBusException as exc:
            return exc.get_dbus_message() or "load failed"
        return ""

    # -- output device ----------------------------------------------------

    def output_device(self) -> str:
        return str(self._get("OutputDevice", ""))

    def output_devices(self) -> list[tuple[str, str]]:
        """(node name, description) for every sink we could send to."""
        raw = self._get("OutputDevices", None)
        if raw is None:
            return []
        out: list[tuple[str, str]] = []
        for entry in raw:
            try:
                out.append((str(entry[0]), str(entry[1])))
            except (TypeError, ValueError, IndexError):
                continue
        return out

    def set_output_device(self, name: str) -> None:
        self._set("OutputDevice", dbus.String(str(name)))

    def quit_daemon(self) -> None:
        """Ask the daemon to shut down. Errors are expected and ignored: the
        connection dies as it goes, which is the point."""
        if self._iface is None:
            return
        try:
            self._iface.Quit()
        except dbus.DBusException:
            pass

    def set_eq_band(self, band: int, db: float) -> None:
        if self._iface is None:
            return
        try:
            self._iface.SetEqBand(dbus.UInt32(int(band)), dbus.Double(float(db)))
        except dbus.DBusException:
            log.exception("SetEqBand failed")
