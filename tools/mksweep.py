# Semibreve — generates the test signal used by run_checks.sh: a 4-second
# 30 Hz-16 kHz log sweep plus a low tone, stereo float32, so every EQ band and
# every effect is excited. Deterministic, so differential runs compare cleanly.
import struct, math, sys
sr, secs, ch = 48000, 4, 2
n = sr*secs
data = bytearray()
for i in range(n):
    t = i/sr
    # sweep 30 Hz -> 16 kHz plus a little noise-ish content, so every band is excited
    f = 30.0 * (16000.0/30.0) ** (t/secs)
    ph = 2*math.pi*30.0*secs/math.log(16000.0/30.0)*((16000.0/30.0)**(t/secs)-1)
    v = 0.4*math.sin(ph) + 0.05*math.sin(2*math.pi*55.0*t)
    l = max(-1.0, min(1.0, v))
    r = max(-1.0, min(1.0, v*0.9 + 0.05*math.sin(2*math.pi*997.0*t)))
    data += struct.pack('<ff', l, r)
hdr = b'RIFF' + struct.pack('<I', 36+len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH',16,3,ch,sr,sr*ch*4,ch*4,32) + b'data' + struct.pack('<I', len(data))
open(sys.argv[1],'wb').write(hdr+bytes(data))
print("wrote", sys.argv[1], n, "frames")
