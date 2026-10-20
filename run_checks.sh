#!/bin/bash
# Semibreve — Milestone 0 verification.
#
# There is no Windows reference to null-test against, and deliberately so:
# both builds compile the same C, so the coefficients cannot differ. What a
# port can plausibly break is everything around the arithmetic. These check
# that.
#
#   ./run_checks.sh        full run (rebuilds, ~1 min)
#   ./run_checks.sh quick  skip the optimisation differential
set -u
cd "$(dirname "$(readlink -f "$0")")"
QUICK=${1:-}
FAILED=0
ok()   { printf '  \033[32mpass\033[0m  %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAILED=1; }

echo "== build =="
./build.sh >/tmp/sb_build.log 2>&1 && ok "libdfxdsp.so links (--no-undefined)" \
    || { bad "build failed — see /tmp/sb_build.log"; exit 1; }

for t in smoke preset_check dfxproc; do
    g++ -O2 -std=c++17 -I dsp/include "tools/$t.cpp" -o "build/$t" \
        -L build -ldfxdsp -Wl,-rpath,'$ORIGIN' 2>/dev/null \
        || { bad "tools/$t.cpp did not build"; exit 1; }
done
ok "harnesses built"

[ -f build/sweep.wav ] || python3 tools/mksweep.py build/sweep.wav >/dev/null

echo "== correctness =="
./build/smoke >/tmp/sb_smoke.log 2>&1 && ok "smoke: bypass bit-exact, output finite" \
    || bad "smoke test — see /tmp/sb_smoke.log"

./build/dfxproc build/sweep.wav /tmp/sb_null.wav --bypass >/dev/null 2>&1
cmp -s build/sweep.wav /tmp/sb_null.wav \
    && ok "bypass null test: output byte-identical to input" \
    || bad "bypass is not a true null"

N=0; BADP=0
for f in presets/factory/*.fac presets/bonus/*.fac; do
    out=$(./build/preset_check "$f" 2>&1)
    lp=$(echo "$out" | grep 'loadPreset ->'   | awk '{print $3}')
    nf=$(echo "$out" | grep 'non-finite'      | awk '{print $2}')
    ch=$(echo "$out" | grep 'samples changed' | awk '{print $3}')
    N=$((N+1))
    [ "$lp" = 0 ] && [ "$nf" = 0 ] && [ "$ch" != 0 ] || { BADP=$((BADP+1)); echo "        $(basename "$f"): load=$lp changed=$ch nonfinite=$nf"; }
done
[ "$BADP" = 0 ] && ok "all $N presets load, apply, process, stay finite" \
                || bad "$BADP of $N presets"

# processAudio() must not call processTimer(). It did upstream, which put a
# mutex and a map walk inside what is now PipeWire's callback. A source check
# rather than a behavioural one on purpose: with our in-process registry the
# call is inert, so restoring it would change no output at all and no audio
# test could see it.
BODY=$(sed -n '/^int DfxDspPrivate::processAudio/,/^}/p' dsp/DfxDspPrivate.cpp)
case "$BODY" in
    *"processTimer();"*) bad "processTimer() is back inside processAudio()" ;;
    *)                   ok "processAudio() is free of processTimer()" ;;
esac

# Every caller must therefore drive it from the control thread instead. Miss it
# and the preset silently never reaches the filters. Match the call and not the
# bare name: all four files discuss processTimer() in a comment, so a looser
# pattern passes whether or not the call is actually there.
MISSING=
for t in smoke preset_check dfxproc; do
    grep -qE '\.processTimer\(\);' "tools/$t.cpp" || MISSING="$MISSING $t"
done
grep -qE '\.processTimer\(\);' daemon/semibreved.cpp || MISSING="$MISSING semibreved"
[ -z "$MISSING" ] && ok "all callers drive processTimer() from the control thread" \
                  || bad "no control-thread processTimer():$MISSING"

# Output must not depend on how the stream is chopped into buffers. PipeWire
# renegotiates the quantum at run time, so a filter whose output changes with
# the block size is broken by definition. This failed before the stereo-flag
# slot-index fix in Play32.c — every block size gave different audio — and
# it is the sharpest guard we have against that whole bug class, because it
# fails loudly where peak and RMS do not move at all.
BSDIFF=0
./build/dfxproc build/sweep.wav /tmp/sb_bs_1024.wav presets/bonus/Jazz.fac >/dev/null 2>&1
for B in 256 512 2048; do
    sed "s/const size_t BLOCK = 1024;/const size_t BLOCK = $B;/" tools/dfxproc.cpp > /tmp/sb_bs_$B.cpp
    g++ -O2 -std=c++17 -I dsp/include /tmp/sb_bs_$B.cpp -o /tmp/sb_bs_$B \
        -L build -ldfxdsp -Wl,-rpath,"$PWD/build" 2>/dev/null \
        || { bad "block-size harness did not build"; BSDIFF=1; break; }
    /tmp/sb_bs_$B build/sweep.wav /tmp/sb_bs_$B.wav presets/bonus/Jazz.fac >/dev/null 2>&1
    cmp -s /tmp/sb_bs_1024.wav /tmp/sb_bs_$B.wav || { BSDIFF=1; echo "        block $B differs from 1024"; }
done
[ "$BSDIFF" = 0 ] && ok "output bit-identical across 256/512/1024/2048 blocks" \
                  || bad "output depends on block size"

# All effects at zero must give unity boost less the Maximizer's -0.3 dB output
# ceiling. This is what caught the LP64 struct misalignment (see codedefs.h): it
# read 0.1634 against an input peak of 0.2500.
T=$(./build/smoke 2>/dev/null | grep 'transparency')
case "$T" in
    *PASS*) ok "transparency with all effects at zero (${T##*-> })" ;;
    *)      bad "transparency: $T" ;;
esac

if [ "$QUICK" != quick ]; then
    echo "== differential =="
    # Four codegens agreeing bit-for-bit means no uninitialised read or UB is
    # reaching the output. Stands in for a second compiler; clang is not
    # installed here, and adding it would strengthen this.
    for O in -O0 -O1 -O2 -O3; do
        d="build-opt${O#-}"
        OUT="$d" OPT="$O" ./build.sh >/dev/null 2>&1
        g++ -O2 -std=c++17 -I dsp/include tools/dfxproc.cpp -o "$d/dfxproc" \
            -L "$d" -ldfxdsp -Wl,-rpath,"$PWD/$d" 2>/dev/null
    done
    DIFF=0
    for p in presets/bonus/Jazz.fac presets/factory/6.fac presets/bonus/Metal.fac; do
        n=$(basename "$p" .fac)
        for O in O0 O1 O2 O3; do
            "build-opt$O/dfxproc" build/sweep.wav "/tmp/sb_${n}_$O.wav" "$p" >/dev/null 2>&1
        done
        for O in O1 O2 O3; do
            cmp -s "/tmp/sb_${n}_O0.wav" "/tmp/sb_${n}_$O.wav" || { DIFF=1; echo "        $n: O0 != $O"; }
        done
    done
    rm -rf build-optO0 build-optO1 build-optO2 build-optO3
    [ "$DIFF" = 0 ] && ok "-O0/-O1/-O2/-O3 bit-identical on 3 presets" \
                    || bad "optimisation levels disagree — undefined behaviour"
fi

echo
[ "$FAILED" = 0 ] && echo "all checks passed" || echo "SOME CHECKS FAILED"
exit $FAILED
