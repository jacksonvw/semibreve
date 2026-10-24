#!/bin/bash
#
# Semibreve — layer 2 verification. Milestone 1's null test.
#
# Deliberately NOT part of run_checks.sh. That suite is hermetic: it builds and
# processes files. This one starts a daemon, mutates the live PipeWire graph and
# plays audio, so it has to be run on purpose.
#
#   ./tools/live_check.sh
#
# What it proves, and why this shape:
#
#   With --bypass the node must pass audio through untouched, so the captured
#   output has to be bit-identical to what was played in. That is a genuine null
#   test of the whole path — PulseAudio client, the Audio/Sink stream, the SPSC
#   ring, the playback stream — with no reference implementation needed and no
#   judgement call about what "sounds right" means. Anything the port breaks in
#   layer 2 shows up as a sample that differs.
#
#   It then runs again without --bypass and checks the DSP actually engaged, and
#   a third time with a preset, comparing what comes out of the live graph
#   against dfxproc's offline output for the same preset. That comparison is
#   only meaningful because the DSP has been block-size independent since
#   the Play32.c stereo-flag fix — before that, the quantum PipeWire happened to pick would
#   have changed the result on its own. It is the end-to-end check for preset
#   loading: it covers the whole path at once and fails on any real error in
#   layer 2. See the note above that check for why it is not bit-exact.
#
# The node's output is unlinked from the hardware sink for the duration, so this
# makes no sound. Links are not restored afterwards because the daemon is
# stopped at the end and they go with it.
#
set -u
cd "$(dirname "$(readlink -f "$0")")/.."
PROJ=$PWD
TMP=$(mktemp -d)
trap 'restore_gain; pkill -f "$PROJ/build/semibreved" 2>/dev/null; pkill -f pw-record 2>/dev/null; rm -rf "$TMP"' EXIT

# The comparisons assume unity gain into the sink, and Semibreve's own volume
# is wherever the desktop's volume control last left it: as the default output
# it is the device that control moves, and WirePlumber restores it. At 40% the
# bypass null test failed on every frame. So measure at 100% and unmuted, and
# put the user's setting back before the sink goes, or WirePlumber would keep
# 100% as theirs.
SAVED_GAIN=
unity_gain() {
    [ -n "$SAVED_GAIN" ] && return 0
    SAVED_GAIN=$(pactl -f json list sinks | python3 -c '
import json, sys
for s in json.load(sys.stdin):
    if s["name"] == "semibreve":
        print(" ".join(str(v["value"]) for v in s["volume"].values()), int(s["mute"]))')
    pactl set-sink-volume semibreve 100%
    pactl set-sink-mute semibreve 0
}
restore_gain() {
    [ -n "$SAVED_GAIN" ] || return 0
    pactl set-sink-volume semibreve ${SAVED_GAIN% *} 2>/dev/null
    pactl set-sink-mute semibreve "${SAVED_GAIN##* }" 2>/dev/null
    SAVED_GAIN=
    sleep 1         # time for WirePlumber to store it before the sink goes
}

ok()  { printf '  \033[32mpass\033[0m  %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAILED=1; }
FAILED=0

[ -x build/semibreved ] || { echo "build/semibreved missing — run ./build.sh"; exit 1; }
command -v paplay   >/dev/null || { echo "paplay not found";   exit 1; }
command -v pw-record >/dev/null || { echo "pw-record not found"; exit 1; }

# A tone we can compare against exactly: 300 Hz, 0.5 amplitude, stereo f32.
python3 - "$TMP/tone.wav" <<'EOF'
import struct, sys, math
N, SR = 48000, 48000
d=b''.join(struct.pack('<ff', 0.5*math.sin(2*math.pi*300*i/SR),
                              0.5*math.sin(2*math.pi*300*i/SR)) for i in range(N))
h=b'RIFF'+struct.pack('<I',36+len(d))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,3,2,SR,SR*8,8,32)+b'data'+struct.pack('<I',len(d))
open(sys.argv[1],'wb').write(h+d)
EOF

# Compare a capture against a reference.
# $1 = capture, $2 = reference, $3 = minimum frames, $4 = tolerance
#
# The alignment sweep is wide on purpose. The capture starts wherever pw-record
# happened to be when playback began, and a processed signal's first audible
# sample is not its first non-zero one, so keying off "first non-silent" alone
# lands tens of frames out. Getting this wrong reports 0/48000 and looks like a
# total failure rather than a misalignment — which is exactly what it did on the
# first attempt.
compare() {
    python3 - "$1" "$2" "$3" "$4" <<'EOF'
import struct, sys
def rd(p):
    b=open(p,'rb').read(); i=12
    while i+8<=len(b):
        c=b[i:i+4]; n=struct.unpack('<I',b[i+4:i+8])[0]
        if c==b'data':
            o=i+8; m=min(n,len(b)-o); return struct.unpack('<%df'%(m//4), b[o:o+(m//4)*4])
        i+=8+n+(n&1)
    return ()
cap=rd(sys.argv[1])[0::2]; ref=rd(sys.argv[2])[0::2]
need=int(sys.argv[3]); tol=float(sys.argv[4])
nz=[i for i,x in enumerate(cap) if abs(x)>1e-9]
if not nz:
    print("        no audio reached the capture"); sys.exit(1)
st=nz[0]
best=(None,None)
for off in range(max(0,st-256), st+257):
    n=min(4000, len(cap)-off, len(ref))
    if n<4000: break
    d=sum(abs(cap[off+i]-ref[i]) for i in range(n))
    if best[1] is None or d<best[1]: best=(off,d)
off=best[0]
n=min(len(cap)-off, len(ref))
exact=sum(1 for i in range(n) if cap[off+i]==ref[i])
mx=max((abs(cap[off+i]-ref[i]) for i in range(n)), default=0.0)
print(f"        {exact}/{n} frames bit-identical, largest difference {mx:.3e}")
ok = n>=need and mx<=tol and exact >= n*0.999
sys.exit(0 if ok else 1)
EOF
}

run_once() {   # $1 = extra daemon args, $2 = capture path
    local args=$1 cap=$2
    pkill -f "$PROJ/build/semibreved" 2>/dev/null; pkill -f pw-record 2>/dev/null; sleep 1

    "$PROJ/build/semibreved" $args > "$TMP/daemon.log" 2>&1 &
    sleep 3

    pactl list short sinks | grep -q '\bsemibreve\b' || { echo "        sink never appeared"; return 1; }
    unity_gain

    # keep the test silent: the output side auto-connects to the default sink
    local hw
    hw=$(pactl get-default-sink)
    pw-link -d semibreve-output:output_FL "$hw:playback_FL" 2>/dev/null
    pw-link -d semibreve-output:output_FR "$hw:playback_FR" 2>/dev/null

    pw-record --target=0 --channels=2 --rate=48000 --format=f32 "$cap" 2>/dev/null &
    sleep 2
    pw-link semibreve-output:output_FL pw-record:input_FL 2>/dev/null
    pw-link semibreve-output:output_FR pw-record:input_FR 2>/dev/null
    sleep 1

    PULSE_SINK=semibreve paplay "$TMP/tone.wav"
    sleep 1
    pkill -f pw-record 2>/dev/null; sleep 1
    restore_gain
    pkill -f "$PROJ/build/semibreved" 2>/dev/null; sleep 1
    return 0
}

echo "== layer 2: the live audio path =="

if run_once "--bypass" "$TMP/bypass.wav"; then
    compare "$TMP/bypass.wav" "$TMP/tone.wav" 40000 0 \
        && ok "bypass is a true null through the live graph" \
        || bad "bypass altered the signal in the live graph"
else
    bad "bypass run did not complete"
fi

if run_once "" "$TMP/proc.wav"; then
    python3 - "$TMP/proc.wav" <<'EOF'
import struct, sys
def rd(p):
    b=open(p,'rb').read(); i=12
    while i+8<=len(b):
        c=b[i:i+4]; n=struct.unpack('<I',b[i+4:i+8])[0]
        if c==b'data':
            o=i+8; m=min(n,len(b)-o); return struct.unpack('<%df'%(m//4), b[o:o+(m//4)*4])
        i+=8+n+(n&1)
    return ()
cap=rd(sys.argv[1])[0::2]
pk=max((abs(x) for x in cap), default=0)
nz=sum(1 for x in cap if abs(x)>1e-9)
print(f"        peak {pk:.6f} from an input peak of 0.5, {nz} non-silent frames")
# The input peaks at 0.5. Anything at or near 0.5 means the DSP did not engage;
# the Maximizer's -0.3 dB ceiling puts a processed tone near 0.9661.
sys.exit(0 if nz > 20000 and pk > 0.6 else 1)
EOF
    [ $? = 0 ] && ok "DSP is applied to audio routed through the sink" \
               || bad "audio passed through but the DSP did not engage"
else
    bad "processing run did not complete"
fi

# Milestone 2: a preset, end to end, against the offline reference.
#
# The reference is generated from silence-then-tone rather than the tone alone.
# The daemon has been processing silence for seconds before playback starts, and
# the DSP's decaying tails never quite reach zero, so its state at the moment
# audio arrives holds a residue of order 1e-24. Feeding the offline run the same
# history reproduces it. Exact bit-identity is still not reachable — the residue
# depends on precisely how long the daemon idled, which a live test cannot fix —
# so the assertion is that every remaining difference sits below 1e-20, roughly
# 400 dB under anything audible, and that at least 99.9% of frames match
# exactly. That is tight enough to catch any real error in layer 2.
PRESET=presets/bonus/Jazz.fac
if [ -x build/dfxproc ] && [ -f "$PRESET" ]; then
    python3 - "$TMP/warm.wav" <<'EOF'
import struct, sys, math
SR=48000
f=[0.0]*(6*SR) + [0.5*math.sin(2*math.pi*300*i/SR) for i in range(SR)]
d=b''.join(struct.pack('<ff',x,x) for x in f)
h=b'RIFF'+struct.pack('<I',36+len(d))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,3,2,SR,SR*8,8,32)+b'data'+struct.pack('<I',len(d))
open(sys.argv[1],'wb').write(h+d)
EOF
    ./build/dfxproc "$TMP/warm.wav" "$TMP/warmref.wav" "$PRESET" >/dev/null 2>&1
    # keep only the tone portion of the reference
    python3 - "$TMP/warmref.wav" "$TMP/ref.wav" <<'EOF'
import struct, sys
b=open(sys.argv[1],'rb').read(); i=12
while i+8<=len(b):
    c=b[i:i+4]; n=struct.unpack('<I',b[i+4:i+8])[0]
    if c==b'data':
        o=i+8; m=min(n,len(b)-o); s=b[o:o+m]; break
    i+=8+n+(n&1)
d=s[6*48000*8:]
h=b'RIFF'+struct.pack('<I',36+len(d))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,3,2,48000,48000*8,8,32)+b'data'+struct.pack('<I',len(d))
open(sys.argv[2],'wb').write(h+d)
EOF
    if run_once "--preset Jazz" "$TMP/preset.wav"; then
        compare "$TMP/preset.wav" "$TMP/ref.wav" 40000 1e-20 \
            && ok "preset through the live graph matches the offline reference" \
            || bad "live preset output differs from the offline reference"
    else
        bad "preset run did not complete"
    fi
else
    echo "        (skipped preset check: build/dfxproc or $PRESET missing)"
fi

grep -E 'underruns|overruns' "$TMP/daemon.log" 2>/dev/null | sed 's/^/        /'

echo
[ "$FAILED" = 0 ] && echo "live path OK" || echo "LIVE PATH FAILED"
exit $FAILED
