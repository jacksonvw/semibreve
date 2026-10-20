"""Platform-aware application directories.

Uses ``platformdirs`` so this module stays Qt-free. Semibreve only ever needs
the config directory — there is nothing to cache and nothing to store — so
this is the whole of it.
"""

from __future__ import annotations

from pathlib import Path

from platformdirs import user_config_dir

APP_NAME = "semibreve"


def config_dir() -> Path:
    """``~/.config/semibreve`` on Linux, created on first access."""
    p = Path(user_config_dir(APP_NAME))
    p.mkdir(parents=True, exist_ok=True)
    return p


def config_path() -> Path:
    """``~/.config/semibreve/config.toml``."""
    return config_dir() / "config.toml"
