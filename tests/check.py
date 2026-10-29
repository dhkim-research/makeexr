# check.py test.hdr exrdump-output.txt : the EXR must hold 179 x RGB->XYZ(PRIMARIES) / EXPOSURE.
import math, sys

data = open(sys.argv[1], "rb").read()
head, rest = data.split(b"\n\n", 1)
res, pix = rest.split(b"\n", 1)
prim = exposure = None
for line in head.decode().splitlines():
    if line.startswith("PRIMARIES="):
        prim = [float(v) for v in line[10:].split()]
    if line.startswith("EXPOSURE="):
        exposure = float(line[9:])
_, h, _, w = res.split()
w, h = int(w), int(h)

# RGB -> XYZ from the chromaticities, white with Y = 1
xr, yr, xg, yg, xb, yb, xw, yw = prim
P = [[xr, xg, xb], [yr, yg, yb], [1 - xr - yr, 1 - xg - yg, 1 - xb - yb]]
Wt = [xw / yw, 1.0, (1 - xw - yw) / yw]
def det(m):
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
S = []
for k in range(3):  # Cramer's rule for P S = Wt
    m = [row[:] for row in P]
    for i in range(3):
        m[i][k] = Wt[i]
    S.append(det(m) / det(P))
M = [[P[i][j] * S[j] for j in range(3)] for i in range(3)]

expected = []
for i in range(w * h):
    r, g, b, e = pix[4 * i: 4 * i + 4]
    f = math.ldexp(1.0, e - 136) if e else 0.0
    rgb = [(r + 0.5) * f, (g + 0.5) * f, (b + 0.5) * f] if e else [0.0, 0.0, 0.0]
    expected.append([179.0 / exposure * sum(M[k][j] * rgb[j] for j in range(3)) for k in range(3)])

lines = open(sys.argv[2]).read().split("pixels\n", 1)
header, values = lines[0], lines[1].split()
assert "channel X type 2" in header and "channel Y type 2" in header and "channel Z type 2" in header, header
got = [float(v) for v in values]
assert len(got) == 3 * w * h, (len(got), w, h)
worst = max(abs(got[3 * i + k] - expected[i][k]) / max(abs(expected[i][k]), 1e-6)
            for i in range(w * h) for k in range(3))
print(f"max relative difference {worst:.2e}; brightest Y {max(e[1] for e in expected):.4g} cd/m2")
sys.exit(0 if worst < 1e-6 else 1)
