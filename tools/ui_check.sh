#!/bin/bash
#
# Semibreve — layer 3 against a live daemon. Milestone 4.
#
#   ./tools/ui_check.sh
#
# Runs the real MainWindow on Qt's offscreen platform with semibreved running,
# and drives it the way a person would: read what it picked up, move a slider,
# flip the power button, choose a preset, and check each change reached the
# daemon and came back.
#
# Offscreen because the window cannot be looked at from a script — but the
# widgets, signals and D-Bus round trips are all real. What this cannot judge
# is whether it looks right, which is the one thing left for a human.
#
# No audio is played, so this is silent.
#
set -u
cd "$(dirname "$(readlink -f "$0")")/.."
PROJ=$PWD
# A config of its own. On connecting, the window restores whatever preset,
# effects and device the user last had, over the daemon's --preset Jazz, so
# against a real ~/.config the result depended on what was saved there.
CFG=$(mktemp -d)
trap 'pkill -f "$PROJ/build/semibreved" 2>/dev/null; rm -rf "$CFG"' EXIT

[ -x build/semibreved ] || { echo "build/semibreved missing — run ./build.sh"; exit 1; }

echo "== layer 3: the UI against a live daemon =="

pkill -f "$PROJ/build/semibreved" 2>/dev/null; sleep 1
"$PROJ/build/semibreved" --preset Jazz > /tmp/semibreve_uicheck_daemon.log 2>&1 &
sleep 3

grep -q "on the session bus" /tmp/semibreve_uicheck_daemon.log || {
    echo "  FAIL  daemon did not claim the bus"; exit 1; }

XDG_CONFIG_HOME="$CFG" QT_QPA_PLATFORM=offscreen PYTHONPATH="$PROJ" python3 - <<'EOF'
import sys
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QTimer, QEventLoop

app = QApplication(sys.argv)
from semibreve.ui.main_window import MainWindow

fails = []
def ok(msg):   print(f"  \033[32mpass\033[0m  {msg}")
def bad(msg):  print(f"  \033[31mFAIL\033[0m  {msg}"); fails.append(msg)

def spin(ms):
    """Let the Qt loop run so D-Bus replies and signals actually arrive."""
    loop = QEventLoop()
    QTimer.singleShot(ms, loop.quit)
    loop.exec()

w = MainWindow()
w.show()
spin(1500)

d = w._daemon
if d.available: ok("UI connected to the daemon")
else:           bad("UI did not see the daemon"); print("no point continuing"); sys.exit(1)

# Presets populated from the daemon, not hardcoded.
n = w._preset.count()
if n >= 31: ok(f"preset list populated from the daemon ({n} entries)")
else:       bad(f"preset list has {n} entries, expected >= 31")

# The daemon was started with --preset Jazz; the window should be showing it.
if w._preset_note.text() == "Jazz": ok('startup preset shown as "Jazz"')
else: bad(f'preset note reads "{w._preset_note.text()}", expected "Jazz"')

# Every slider should hold what the daemon reports, not a default.
mismatch = [p for p in w._sliders
            if abs(w._sliders[p].value() - d.effect(p)) > 0.01]
if not mismatch: ok("all five sliders match the daemon's values")
else:            bad(f"sliders out of step with the daemon: {mismatch}")

# Move a slider -> the daemon should take it.
w._sliders["Fidelity"].value_changed.emit(6.5)
spin(400)
got = d.effect("Fidelity")
if abs(got - 6.5) < 0.05: ok(f"moving a slider reached the daemon (Fidelity={got:.2f})")
else:                     bad(f"slider change did not land (Fidelity={got:.2f}, wanted 6.5)")

# A change made behind the UI's back should come back through PropertiesChanged.
d.set_effect("Ambience", 2.25)
spin(600)
shown = w._sliders["Ambience"].value()
if abs(shown - 2.25) < 0.05: ok(f"an external change updated the UI (Ambience={shown:.2f})")
else:                        bad(f"UI did not follow an external change (shows {shown:.2f})")

# Power button reflects processing, not bypass.
before = w._power.is_on()
w._power.toggled.emit(not before)
spin(600)
if d.bypass() == before: ok(f"power button drove bypass (on={not before} -> bypass={d.bypass()})")
else:                    bad("power button did not change bypass")
w._power.toggled.emit(before)
spin(600)

# Choosing a preset.
idx = next((i for i in range(w._preset.count())
            if w._preset.itemData(i) == "Metal"), -1)
if idx < 0:
    bad("Metal not in the preset list")
else:
    w._preset.setCurrentIndex(idx)
    w._on_preset_activated(idx)
    spin(1200)
    if d.preset_id() == "Metal": ok(f'preset change applied (now "{d.preset_name()}")')
    else:                        bad(f'preset did not change (daemon says "{d.preset_id()}")')

# The EQ, once shown, should build from the daemon's own band table.
w._eq.setVisible(True)
w._refresh_eq()
spin(400)
bands = d.eq_bands()
built = len(w._eq._bands)
if built == len(bands) and built >= 31:
    ok(f"EQ built {built} bands from the daemon's frequencies")
else:
    bad(f"EQ built {built} bands, daemon reports {len(bands)}")

if built:
    w._eq.band_changed.emit(0, 4.0)
    spin(400)
    got = d.eq_bands()[0][1]
    if abs(got - 4.0) < 0.2: ok(f"an EQ band change reached the daemon ({got:.2f} dB)")
    else:                    bad(f"EQ band change did not land ({got:.2f} dB, wanted 4.0)")

# Losing the daemon must disable the controls rather than throw.
w._on_availability(False)
if not w._power.isEnabled() and w._banner.isVisible():
    ok("controls disable and a banner appears when the daemon goes away")
else:
    bad("UI did not degrade when the daemon went away")

sys.exit(1 if fails else 0)
EOF
RC=$?

echo
[ "$RC" = 0 ] && echo "UI OK" || echo "UI FAILED"
exit $RC
