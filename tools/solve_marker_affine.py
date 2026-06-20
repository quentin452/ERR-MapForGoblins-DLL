#!/usr/bin/env python3
"""Solve the world-map marker affine  render = M*world + T[page]  from captured pairs.

Input CSV (one row per captured grace), columns:
    page,wx,wz,rx,rz
  page  : dst-page label (60,61 overworld / 12 underground / 40-43 DLC)
  wx,wz : grace world = gridXNo*256 + posX , gridZNo*256 + posZ   (from GRACE_ANCHORS)
  rx,rz : render_out  = cursor +0x104 / +0x108 when the cursor snapped onto the grace
          (captured by tools/cheat_engine/MapForGoblins_marker_capture.CT)
Lines starting with '#' are ignored. Need >=3 spread graces per page.

Usage:  python solve_marker_affine.py pairs.csv
Output: per-page M=[[a,b],[c,d]] + T=(e,f), residuals, and whether M is shared across pages.
"""
import sys
from collections import defaultdict

# --- pure-python least squares (no numpy dependency) -------------------------
def _lstsq3(rows, ys):
    """Least-squares solve for [p,q,r] minimizing sum((p*wx+q*wz+r) - y)^2.
    rows = [(wx,wz,1.0)], ys = [y]. Via 3x3 normal equations + Gaussian elim."""
    A = [[0.0] * 3 for _ in range(3)]
    b = [0.0, 0.0, 0.0]
    for (x0, x1, x2), y in zip(rows, ys):
        v = (x0, x1, x2)
        for i in range(3):
            b[i] += v[i] * y
            for j in range(3):
                A[i][j] += v[i] * v[j]
    # Gaussian elimination with partial pivoting on the 3x3 system A·p = b
    M = [A[i][:] + [b[i]] for i in range(3)]
    for col in range(3):
        piv = max(range(col, 3), key=lambda r: abs(M[r][col]))
        if abs(M[piv][col]) < 1e-12:
            raise ValueError("singular (points collinear / too few)")
        M[col], M[piv] = M[piv], M[col]
        pv = M[col][col]
        M[col] = [v / pv for v in M[col]]
        for r in range(3):
            if r != col:
                fac = M[r][col]
                M[r] = [M[r][k] - fac * M[col][k] for k in range(4)]
    return [M[i][3] for i in range(3)]


def load(path):
    rows = defaultdict(list)  # page -> [(wx,wz,rx,rz), ...]
    with open(path, newline="") as f:
        for raw in f:
            s = raw.strip()
            if not s or s.startswith("#"):
                continue
            parts = [p.strip() for p in s.split(",") if p.strip() != ""]
            if len(parts) < 5:
                continue
            try:
                page = int(float(parts[0]))
                wx, wz, rx, rz = (float(parts[i]) for i in range(1, 5))
            except ValueError:
                continue
            rows[page].append((wx, wz, rx, rz))
    return rows


def solve_page(pairs):
    W = [(wx, wz, 1.0) for (wx, wz, _, _) in pairs]
    rx = [r for (_, _, r, _) in pairs]
    rz = [r for (_, _, _, r) in pairs]
    a, b, e = _lstsq3(W, rx)
    c, d, f = _lstsq3(W, rz)
    res = []
    for (wx, wz, _), gx, gz in zip(W, rx, rz):
        px = a * wx + b * wz + e
        pz = c * wx + d * wz + f
        res.append(((px - gx) ** 2 + (pz - gz) ** 2) ** 0.5)
    return (a, b, c, d, e, f), sum(res) / len(res), max(res)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rows = load(sys.argv[1])
    if not rows:
        sys.exit("no usable rows")

    Ms, Ts = {}, {}
    print(f"{'page':>4} {'n':>3}  {'a':>9} {'b':>9} {'c':>9} {'d':>9}   {'Tx(e)':>10} {'Tz(f)':>10}   {'resMean':>8} {'resMax':>8}")
    for page in sorted(rows):
        pairs = rows[page]
        if len(pairs) < 3:
            print(f"{page:>4} {len(pairs):>3}  -- need >=3 pairs, skipping")
            continue
        (a, b, c, d, e, f), rmean, rmax = solve_page(pairs)
        Ms[page] = (a, b, c, d)
        Ts[page] = (e, f)
        print(f"{page:>4} {len(pairs):>3}  {a:9.5f} {b:9.5f} {c:9.5f} {d:9.5f}   {e:10.2f} {f:10.2f}   {rmean:8.3f} {rmax:8.3f}")

    if len(Ms) >= 2:
        keys = list(Ms)
        M0 = Ms[keys[0]]
        spread = max(max(abs(Ms[k][i] - M0[i]) for i in range(4)) for k in keys[1:])
        shared = spread < 1e-3
        print(f"\nM shared across pages: {'YES' if shared else 'NO'}  (max element spread = {spread:.2e})")
        if shared:
            a, b, c, d = M0
            print(f"\n=== BAKE THESE ===\nM = [[{a:.6f}, {b:.6f}], [{c:.6f}, {d:.6f}]]")
            print("T[page] = {")
            for page in sorted(Ts):
                e, f = Ts[page]
                print(f"    {page}: ({e:.3f}, {f:.3f}),")
            print("}")
        else:
            print("M differs per page -> the transform is richer than one global affine; bake per-page M+T.")


if __name__ == "__main__":
    main()
