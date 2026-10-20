#!/bin/bash
# Semibreve — Milestone 0 build. Temporary; folds into pyproject.toml once the
# DSP is verified. CC/CXX are honoured so the same tree builds under clang for
# the cross-compiler differential.
set -e
CC=${CC:-gcc}; CXX=${CXX:-g++}; OPT=${OPT:--O2}
OUT=${OUT:-build}
INC="-I dsp/linux -I dsp/linux/compat -I dsp/ptutil/include -I dsp/include -I dsp -I vendor/audiopassthru/include"
DEF="-DPT_LINUX -DDSPSOFT_TARGET -DPT_NON_MFC -DPT_DSP_BUILD=PT_DSP_DFX"
# No -ffast-math anywhere: bit-identical output across compilers is the check
# that stands in for a null test against Windows.
FLAGS="-fPIC $OPT -fno-fast-math"
mkdir -p "$OUT/obj"
while read f; do
  o="$OUT/obj/$(echo "$f" | tr '/' '_').o"
  if [ "${f##*.}" = "c" ]; then $CC -c $FLAGS $INC $DEF "dsp/$f" -o "$o"
  else $CXX -c $FLAGS -std=c++17 $INC $DEF "dsp/$f" -o "$o"; fi
done < dsp/BUILD_SOURCES.txt
for f in vendor/audiopassthru/SLOUT/Slout.cpp \
         vendor/audiopassthru/src/MTH/*.cpp \
         vendor/audiopassthru/src/FILE/FileGeneral.cpp \
         vendor/audiopassthru/src/pstr/*.cpp \
         vendor/audiopassthru/src/MRY/*.cpp \
         dsp/linux/stubs.cpp; do
  $CXX -c $FLAGS -std=c++17 $INC $DEF "$f" -o "$OUT/obj/$(basename "$f" .cpp).o"
done
# --no-undefined: upstream is a static library and tolerates dangling symbols;
# a shared object must not, or a missing one becomes a crash at run time.
$CXX -shared -Wl,--no-undefined -o "$OUT/libdfxdsp.so" "$OUT"/obj/*.o -lm
echo "built $OUT/libdfxdsp.so"

# ---- layer 2: the PipeWire node -------------------------------------------
# Skipped rather than fatal when pipewire-devel is absent, so the DSP build and
# its verification suite never acquire a dependency on it. -O2 regardless of
# $OPT: the optimisation differential is about the DSP's arithmetic, and
# rebuilding the daemon at -O0 for it would prove nothing.
if pkg-config --exists libpipewire-0.3; then
  # The D-Bus surface degrades the same way the daemon does: no libsystemd
  # headers means a no-op control surface, not a broken build. See
  # daemon/dbus_none.c.
  if pkg-config --exists libsystemd; then
    DBUS_SRC=daemon/dbus.c
    DBUS_CFLAGS=$(pkg-config --cflags libsystemd)
    DBUS_LIBS=$(pkg-config --libs libsystemd)
  else
    DBUS_SRC=daemon/dbus_none.c
    DBUS_CFLAGS=""
    DBUS_LIBS=""
    echo "note: libsystemd headers absent — semibreved will have no D-Bus surface" >&2
  fi

  # Deliberately not under $OUT/obj: that directory is globbed wholesale into
  # libdfxdsp.so, and an object built without -fPIC lands there as a link
  # failure on the *second* build, once the glob can see it.
  $CC -O2 -std=c11 -fno-fast-math -Wall -Wextra \
      -I daemon $DBUS_CFLAGS -c "$DBUS_SRC" -o "$OUT/dbus.o"

  $CXX -O2 -std=c++17 -fno-fast-math -Wall -Wextra \
      -I dsp/include -I daemon $(pkg-config --cflags libpipewire-0.3) \
      daemon/semibreved.cpp "$OUT/dbus.o" -o "$OUT/semibreved" \
      -L "$OUT" -ldfxdsp -Wl,-rpath,'$ORIGIN' \
      $(pkg-config --libs libpipewire-0.3) $DBUS_LIBS
  echo "built $OUT/semibreved"
else
  echo "skipped $OUT/semibreved — pipewire-devel not installed" >&2
fi
