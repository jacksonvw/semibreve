#!/bin/bash
#
# Semibreve — layer 2's control surface. Milestone 3.
#
# Like live_check.sh this starts a daemon and touches the live graph, so it is
# not part of run_checks.sh.
#
#   ./tools/dbus_check.sh
#
# Two halves, and the second is the one that matters.
#
#   The first walks the interface with busctl: read every property, write the
#   writable ones, call every method, and check the readback. That catches a
#   wrong signature or a getter wired to the wrong effect.
#
#   The second changes a preset *while audio is playing* and looks for a
#   discontinuity in the captured output. A preset rewrites some forty
#   parameters and every EQ band, and the audio thread is reading them the
#   whole time. The daemon handles this by ramping its output to zero, applying
#   on a main-loop timer once the ramp has landed, and ramping back up. If that
#   fade is broken, or the timer fires too early, the seam shows up as a step
#   in a signal that has none of its own. That is the whole reason the
#   machinery exists, so it is the thing worth testing.
#
# The node's output is unlinked from the hardware sink throughout, so this
# makes no sound.
#
set -u
cd "$(dirname "$(readlink -f "$0")")/.."
PROJ=$PWD
TMP=$(mktemp -d)
trap 'pkill -f "$PROJ/build/semibreved" 2>/dev/null; pkill -f pw-record 2>/dev/null; rm -rf "$TMP"' EXIT

ok()   { printf '  \033[32mpass\033[0m  %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAILED=1; }
info() { printf '        %s\n' "$1"; }
FAILED=0

BUS="busctl --user"
DEST=org.semibreve.Daemon
OBJ=/org/semibreve/Daemon
IFACE=org.semibreve.Daemon

[ -x build/semibreved ] || { echo "build/semibreved missing — run ./build.sh"; exit 1; }
command -v busctl >/dev/null || { echo "busctl not found"; exit 1; }

getp() { $BUS get-property $DEST $OBJ $IFACE "$1" 2>/dev/null; }
setp() { $BUS set-property $DEST $OBJ $IFACE "$1" "$2" "$3" 2>&1; }

echo "== layer 2: the D-Bus control surface =="

"$PROJ/build/semibreved" > "$TMP/daemon.log" 2>&1 &
sleep 3

if ! grep -q "org.semibreve.Daemon on the session bus" "$TMP/daemon.log"; then
    if grep -q "built without libsystemd" "$TMP/daemon.log"; then
        echo "        skipped: built without libsystemd (install systemd-devel, rebuild)"
        exit 0
    fi
    bad "daemon did not claim the bus name"
    sed 's/^/        /' "$TMP/daemon.log"
    exit 1
fi
ok "daemon claimed org.semibreve.Daemon"

# ---- the interface, walked ------------------------------------------------

if $BUS introspect $DEST $OBJ > "$TMP/introspect.txt" 2>&1; then
    MISSING=
    for m in Bypass Fidelity Ambience Surround DynamicBoost Bass \
             PresetId PresetName EqBands LoadPreset ListPresets SetEqBand; do
        grep -q "\\b$m\\b" "$TMP/introspect.txt" || MISSING="$MISSING $m"
    done
    [ -z "$MISSING" ] && ok "every documented member is present on the interface" \
                      || bad "interface is missing:$MISSING"
else
    bad "introspection failed"
fi

# Each effect must read back what was written, and must reject out-of-range.
EFFECT_FAIL=
for e in Fidelity Ambience Surround DynamicBoost Bass; do
    setp "$e" d 7.25 >/dev/null
    got=$(getp "$e" | awk '{print $2}')
    case "$got" in
        7.25*) ;;
        *) EFFECT_FAIL="$EFFECT_FAIL $e(got=$got)" ;;
    esac
done
[ -z "$EFFECT_FAIL" ] && ok "all five effects read back what was written" \
                      || bad "effect round-trip failed:$EFFECT_FAIL"

# 11 is out of range and must be refused rather than silently clamped.
if setp Fidelity d 11 2>&1 | grep -qi "between 0 and 10"; then
    back=$(getp Fidelity | awk '{print $2}')
    case "$back" in
        7.25*) ok "an out-of-range effect is refused and the old value stands" ;;
        *)     bad "value changed despite the write being refused (now $back)" ;;
    esac
else
    bad "an out-of-range effect value was not refused"
fi

# Bypass round-trip.
setp Bypass b true >/dev/null; sleep 1
b1=$(getp Bypass | awk '{print $2}')
setp Bypass b false >/dev/null; sleep 1
b2=$(getp Bypass | awk '{print $2}')
[ "$b1" = "true" ] && [ "$b2" = "false" ] && ok "bypass toggles both ways" \
                                          || bad "bypass round-trip failed ($b1 then $b2)"

# ListPresets should find all 31 stock presets.
N=$($BUS call $DEST $OBJ $IFACE ListPresets 2>/dev/null | grep -o '"' | wc -l)
N=$((N / 4))     # two quoted strings per entry, each quote counted twice
if [ "$N" -ge 31 ]; then ok "ListPresets returned $N presets"
else bad "ListPresets returned $N presets, expected at least 31"; fi

# LoadPreset, and the reported identity that follows it.
$BUS call $DEST $OBJ $IFACE LoadPreset s Jazz >/dev/null 2>&1
sleep 1
pid=$(getp PresetId   | sed 's/^s //; s/"//g')
pnm=$(getp PresetName | sed 's/^s //; s/"//g')
[ "$pid" = "Jazz" ] && ok "LoadPreset Jazz -> PresetId=\"$pid\" PresetName=\"$pnm\"" \
                    || bad "after LoadPreset Jazz, PresetId=\"$pid\" PresetName=\"$pnm\""

if $BUS call $DEST $OBJ $IFACE LoadPreset s nosuchpreset 2>&1 | grep -qi "no preset named"; then
    ok "an unknown preset is refused with a useful error"
else
    bad "an unknown preset was not refused"
fi

# EQ.
BANDS=$(getp EqBands 2>/dev/null | grep -o '(dd)' | wc -l)
$BUS call $DEST $OBJ $IFACE SetEqBand ud 0 3.0 >/dev/null 2>&1 && SETOK=1 || SETOK=0
if [ "$SETOK" = 1 ]; then ok "EqBands readable, SetEqBand accepted"
else bad "SetEqBand failed"; fi
if $BUS call $DEST $OBJ $IFACE SetEqBand ud 9999 1.0 2>&1 | grep -qi "out of range"; then
    ok "an out-of-range EQ band is refused"
else
    bad "an out-of-range EQ band was not refused"
fi

pkill -f "$PROJ/build/semibreved" 2>/dev/null; sleep 1

# ---- a preset change under audio -----------------------------------------

echo
echo "== a preset change while audio is playing =="

python3 - "$TMP/long.wav" <<'EOF'
import struct, sys, math
SR=48000; N=SR*6
d=b''.join(struct.pack('<ff', 0.4*math.sin(2*math.pi*300*i/SR),
                              0.4*math.sin(2*math.pi*300*i/SR)) for i in range(N))
h=b'RIFF'+struct.pack('<I',36+len(d))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,3,2,SR,SR*8,8,32)+b'data'+struct.pack('<I',len(d))
open(sys.argv[1],'wb').write(h+d)
EOF

"$PROJ/build/semibreved" --preset Jazz > "$TMP/daemon2.log" 2>&1 &
sleep 3
HW=$(pactl get-default-sink)
pw-link -d semibreve-output:output_FL "$HW:playback_FL" 2>/dev/null
pw-link -d semibreve-output:output_FR "$HW:playback_FR" 2>/dev/null
pw-record --target=0 --channels=2 --rate=48000 --format=f32 "$TMP/swap.wav" 2>/dev/null &
sleep 2
pw-link semibreve-output:output_FL pw-record:input_FL 2>/dev/null
pw-link semibreve-output:output_FR pw-record:input_FR 2>/dev/null
sleep 1

PULSE_SINK=semibreve paplay "$TMP/long.wav" &
PLAY=$!
sleep 2
$BUS call $DEST $OBJ $IFACE LoadPreset s Metal >/dev/null 2>&1
sleep 2
$BUS call $DEST $OBJ $IFACE LoadPreset s Vocal >/dev/null 2>&1 || \
$BUS call $DEST $OBJ $IFACE LoadPreset s Rock  >/dev/null 2>&1
wait $PLAY 2>/dev/null
sleep 1
pkill -f pw-record 2>/dev/null; sleep 1
pkill -f "$PROJ/build/semibreved" 2>/dev/null; sleep 1

python3 - "$TMP/swap.wav" <<'EOF'
import struct, sys
def rd(p):
    b=open(p,'rb').read(); i=12
    while i+8<=len(b):
        c=b[i:i+4]; n=struct.unpack('<I',b[i+4:i+8])[0]
        if c==b'data':
            o=i+8; m=min(n,len(b)-o); return struct.unpack('<%df'%(m//4), b[o:o+(m//4)*4])
        i+=8+n+(n&1)
    return ()
s=rd(sys.argv[1])[0::2]
live=[i for i,x in enumerate(s) if abs(x)>1e-6]
# The tone is 6 s = 288000 frames and both swaps happen inside it. Demand most
# of it: a short capture can miss the swaps entirely and then the step check
# below is measuring nothing. A run that captured 92505 frames passed happily
# while proving nothing at all, which is how this bound got here.
if len(live) < 240000:
    print(f"        only {len(live)} frames captured of ~288000 — "
          f"the swaps may not be in the capture; not a verdict")
    sys.exit(1)
a,b = live[0], live[-1]
# The threshold is calibrated against measurement, and it took three goes.
#
#   0.15  picked from the *input* signal's smoothness. Passed at 0.1311 while
#         the DSP's post-change transient was going straight through.
#   0.08  picked from a steady processed tone, which measures 0.0473 max and
#         0.0411 at the 99.99th percentile. Correct in kind, still too loose:
#         removing the post-apply hold gives 0.0695, and 0.08 let that pass.
#   0.055 above the steady signal's own maximum with a little headroom, and
#         comfortably below the 0.0695 that a missing hold produces.
#
# Verified in both directions: with HOLD_MS at its shipped 40 ms this reads
# 0.0473, and with the hold removed it reads 0.0695 and fails.
LIMIT=0.055
steps=[abs(s[i]-s[i-1]) for i in range(a+1, b+1)]
mx=max(steps); big=sum(1 for d in steps if d > LIMIT)
print(f"        {b-a} frames, largest sample step {mx:.4f} "
      f"(steady signal measures 0.0473), steps>{LIMIT}: {big}")
sys.exit(0 if big == 0 else 1)
EOF
SWAP_RC=$?

# An xrun inserts silence, which is a step, and this check cannot tell that
# apart from a fade that failed. The daemon counts them, so ask: if the graph
# glitched, the capture is not evidence either way and saying so is better than
# blaming the fade. Underruns are rare — most runs report none — so this
# normally still gives a verdict.
UNDER=$(sed -n 's/.*, \([0-9]*\) underruns.*/\1/p' "$TMP/daemon2.log" | tail -1)
UNDER=${UNDER:-0}
if [ "$SWAP_RC" != 0 ] && [ "$UNDER" != 0 ]; then
    info "inconclusive: the daemon reported $UNDER underrun(s), so the step"
    info "above is as likely to be the xrun as the preset change. Re-run."
elif [ "$SWAP_RC" = 0 ]; then
    ok "presets swapped under audio with no discontinuity"
else
    bad "a preset change produced a step in the output"
fi

# ---- do the effect knobs actually reach the DSP? -------------------------
#
# A readback cannot answer this. setEffectValue() stores its argument in
# DfxDsp's own struct field *and* forwards a scaled copy to the knob; if the
# scaling is wrong the knob rejects it out of range, the struct field keeps the
# value, and getEffectValue() hands it straight back. That is exactly the bug
# this check was written for — the round trip looked perfect while the DSP
# never saw the value. So: play the same tone twice, with the effects at zero
# and at ten, and require the audio to differ.

echo
echo "== the effect knobs, measured in the audio =="

capture_with_effects() {   # $1 = value for all five, $2 = capture path
    local v=$1 cap=$2
    pkill -f "$PROJ/build/semibreved" 2>/dev/null; pkill -f pw-record 2>/dev/null; sleep 1
    "$PROJ/build/semibreved" > "$TMP/daemon3.log" 2>&1 &
    sleep 3
    for e in Fidelity Ambience Surround DynamicBoost Bass; do
        $BUS set-property $DEST $OBJ $IFACE "$e" d "$v" >/dev/null 2>&1
    done
    sleep 1
    local hw; hw=$(pactl get-default-sink)
    pw-link -d semibreve-output:output_FL "$hw:playback_FL" 2>/dev/null
    pw-link -d semibreve-output:output_FR "$hw:playback_FR" 2>/dev/null
    pw-record --target=0 --channels=2 --rate=48000 --format=f32 "$cap" 2>/dev/null &
    sleep 2
    pw-link semibreve-output:output_FL pw-record:input_FL 2>/dev/null
    pw-link semibreve-output:output_FR pw-record:input_FR 2>/dev/null
    sleep 1
    PULSE_SINK=semibreve paplay "$TMP/long.wav"
    sleep 1
    pkill -f pw-record 2>/dev/null; sleep 1
    pkill -f "$PROJ/build/semibreved" 2>/dev/null; sleep 1
}

capture_with_effects 0  "$TMP/fx_lo.wav"
capture_with_effects 10 "$TMP/fx_hi.wav"

python3 - "$TMP/fx_lo.wav" "$TMP/fx_hi.wav" <<'EOF'
import struct, sys, math
def rd(p):
    b=open(p,'rb').read(); i=12
    while i+8<=len(b):
        c=b[i:i+4]; n=struct.unpack('<I',b[i+4:i+8])[0]
        if c==b'data':
            o=i+8; m=min(n,len(b)-o); return struct.unpack('<%df'%(m//4), b[o:o+(m//4)*4])
        i+=8+n+(n&1)
    return ()
def live(s):
    nz=[i for i,x in enumerate(s) if abs(x)>1e-6]
    return s[nz[0]:nz[-1]] if nz else ()
lo=live(rd(sys.argv[1])[0::2]); hi=live(rd(sys.argv[2])[0::2])
if len(lo) < 20000 or len(hi) < 20000:
    print(f"        too little audio ({len(lo)}, {len(hi)}) to judge"); sys.exit(1)
def rms(s): return math.sqrt(sum(x*x for x in s)/len(s))
rl, rh = rms(lo), rms(hi)
ratio = rh/rl if rl else 0.0
print(f"        effects at 0: rms {rl:.4f}   at 10: rms {rh:.4f}   ratio {ratio:.3f}")
# Turning every effect from nothing to full has to be audible. Anything within
# a few percent means the knobs are not reaching the DSP.
sys.exit(0 if abs(ratio - 1.0) > 0.05 else 1)
EOF
[ $? = 0 ] && ok "effect knobs change the audio, not just the readback" \
           || bad "effects at 0 and 10 produced the same audio — knobs are not reaching the DSP"

grep -E 'underruns|overruns' "$TMP/daemon2.log" 2>/dev/null | sed 's/^/        /'

echo
[ "$FAILED" = 0 ] && echo "control surface OK" || echo "CONTROL SURFACE FAILED"
exit $FAILED
