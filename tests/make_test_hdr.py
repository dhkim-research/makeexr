# Writes test.hdr: a 64 x 48 Radiance picture (flat RGBE, Radiance primaries, EXPOSURE=0.5)
# with a smooth gradient and one very bright "sun" pixel, for the CI test.
import math, sys

W, H = 64, 48

def rgb(r, c):
    if (r, c) == (10, 20):
        return (5.0e6, 4.0e6, 3.0e6)      # above the half-float range once x 179
    return (0.01 * (c + 1), 0.02 * (r + 1), 0.5)

def rgbe(v):
    m = max(v)
    if m < 1e-32:
        return bytes(4)
    e = math.frexp(m)[1]
    f = 256.0 / 2.0 ** e
    return bytes([int(x * f) for x in v] + [e + 128])

out = bytearray(b"#?RADIANCE\nPRIMARIES= 0.640 0.330 0.290 0.600 0.150 0.060 0.333 0.333\n"
                b"EXPOSURE=0.5\nFORMAT=32-bit_rle_rgbe\n\n")
out += f"-Y {H} +X {W}\n".encode()
for r in range(H):
    for c in range(W):
        out += rgbe(rgb(r, c))
open(sys.argv[1], "wb").write(out)
