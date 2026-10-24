"""Semibreve application configuration — TOML, no PySide6 imports.

What lives here and what deliberately does not.

The daemon owns the audio state: which preset is loaded, where the five
effects sit, whether bypass is on. The UI reads that over D-Bus and never
second-guesses it, because the daemon can be driven by ``busctl`` or a second
client and the UI is not the only thing with an opinion. Persisting effect
values here as well would give two sources of truth that quietly diverge, and
the one the user hears would be the daemon's.

So this file holds only what the daemon has no notion of: how the window
looks, and which preset to ask for at startup.
"""

from __future__ import annotations

import tomllib
from dataclasses import asdict, dataclass, field

import tomli_w

from semibreve.core.paths import config_path


@dataclass
class AppearanceConfig:
    """The ``[appearance]`` section: how the window looks."""
    window_opacity: float = 1.0
    blur_behind: bool = True
    bg_color: list[int] = field(default_factory=lambda: [0, 0, 0])
    bg_tint_strength: int = 70


@dataclass
class SoundConfig:
    """What the user last had set.

    The daemon is started and stopped with the app now, so its state does not
    survive a quit — which means this has to. Not persisting it was a real
    fault: every launch silently threw away the tuning, and the only way to
    notice was to remember what you had set last time.

    ``preset`` is the identifier, not the display name; effects are the 0..10
    the UI shows; ``eq`` is one gain in dB per band.
    """
    preset: str = ""
    effects: dict[str, float] = field(default_factory=dict)
    eq: list[float] = field(default_factory=list)
    bypass: bool = False
    output_device: str = ""


@dataclass
class AppConfig:
    appearance: AppearanceConfig = field(default_factory=AppearanceConfig)
    sound: SoundConfig = field(default_factory=SoundConfig)
    # Remember the EQ section's expanded state. Thirty-one bands is a lot of
    # window for someone who only ever touches the five knobs.
    show_eq: bool = False


def _clamp_unit(v: float) -> float:
    return max(0.0, min(1.0, v))


def _clamp_byte(v: int) -> int:
    return max(0, min(255, int(v)))


def _clamp_pct(v: int) -> int:
    return max(0, min(100, int(v)))


def _coerce_color(raw) -> list[int]:
    if not isinstance(raw, (list, tuple)) or len(raw) != 3:
        return [0, 0, 0]
    try:
        return [_clamp_byte(int(c)) for c in raw]
    except (TypeError, ValueError):
        return [0, 0, 0]


def _table(parent: dict, key: str) -> dict:
    """``parent[key]`` if it is a table, else an empty one.

    A hand-edited ``sound = 5`` used to stop the app at startup with an
    AttributeError, which is exactly the lock-out load_config promises not to
    cause.
    """
    value = parent.get(key)
    return value if isinstance(value, dict) else {}


def _coerce_appearance(section: dict) -> AppearanceConfig:
    try:
        opacity = float(section.get("window_opacity", 1.0))
    except (TypeError, ValueError):
        opacity = 1.0
    try:
        tint = int(section.get("bg_tint_strength", 70))
    except (TypeError, ValueError):
        tint = 70
    return AppearanceConfig(
        window_opacity=_clamp_unit(opacity),
        blur_behind=bool(section.get("blur_behind", True)),
        bg_color=_coerce_color(section.get("bg_color", [0, 0, 0])),
        bg_tint_strength=_clamp_pct(tint),
    )


def load_config() -> AppConfig:
    """Read the config, falling back to defaults for anything missing.

    A malformed file is not fatal. Losing a window-opacity preference is a
    smaller problem than refusing to start, and the file is hand-editable, so
    a stray character should not lock the user out of their own audio.
    """
    path = config_path()
    if not path.exists():
        return AppConfig()
    try:
        with open(path, "rb") as f:
            data = tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError):
        return AppConfig()

    general = _table(data, "general")
    raw_sound = _table(data, "sound")

    effects: dict[str, float] = {}
    for key, value in _table(raw_sound, "effects").items():
        try:
            effects[str(key)] = max(0.0, min(10.0, float(value)))
        except (TypeError, ValueError):
            continue

    raw_eq = raw_sound.get("eq")
    eq: list[float] = []
    for value in (raw_eq if isinstance(raw_eq, list) else []):
        try:
            eq.append(max(-12.0, min(12.0, float(value))))
        except (TypeError, ValueError):
            eq.append(0.0)

    return AppConfig(
        appearance=_coerce_appearance(_table(data, "appearance")),
        sound=SoundConfig(
            preset=str(raw_sound.get("preset", "")),
            effects=effects,
            eq=eq,
            bypass=bool(raw_sound.get("bypass", False)),
            output_device=str(raw_sound.get("output_device", "")),
        ),
        show_eq=bool(general.get("show_eq", False)),
    )


def save_config(cfg: AppConfig) -> None:
    data = {
        "general": {
            "show_eq": cfg.show_eq,
        },
        "sound": {
            "preset": cfg.sound.preset,
            "effects": cfg.sound.effects,
            "eq": cfg.sound.eq,
            "bypass": cfg.sound.bypass,
            "output_device": cfg.sound.output_device,
        },
        "appearance": asdict(cfg.appearance),
    }
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        tomli_w.dump(data, f)
