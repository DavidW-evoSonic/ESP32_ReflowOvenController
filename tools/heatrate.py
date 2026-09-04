#!/usr/bin/env python3
"""Full-power heating rate per temperature band, from an ovenlog.sh CSV.

    tools/heatrate.py logs/heatrate.csv [band_degC]

Only samples taken at >=95% duty are counted, so easing off near a setpoint
does not masquerade as the oven running out of steam.
"""
import csv, sys
from collections import defaultdict

path = sys.argv[1]
band = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
rows = list(csv.DictReader(open(path)))

pts = []
for r in rows:
    try:
        t = float(r["time"]) / 1000.0          # ms -> s
        pts.append((t, float(r["temp"]), float(r["power"])))
    except (ValueError, KeyError):
        continue

# The element lags the demand. A sample can read 100% duty while the element
# is still cold from a cut a moment earlier, which reads as a collapse in
# capability -- measured 2026-09-04, where a genuine 2.25 degC/s at 210-220
# was followed by an apparent 0.64 at 230-240 purely because the regulator had
# started cycling. So require the demand to have been full for a WHILE.
SETTLE = 3          # consecutive prior samples that must also be full power

buckets = defaultdict(lambda: [0.0, 0.0])      # band -> [degC gained, seconds]
run = 0
for i, ((t0, T0, P0), (t1, T1, _)) in enumerate(zip(pts, pts[1:])):
    run = run + 1 if P0 >= 95.0 else 0
    dt = t1 - t0
    if dt <= 0 or dt > 5:       continue        # gap in the log
    if run <= SETTLE:           continue        # element not yet up to demand
    if T1 <= T0:                continue        # not climbing
    b = int(T0 // band) * band
    buckets[b][0] += T1 - T0
    buckets[b][1] += dt

if not buckets:
    print("no full-power climbing samples found")
    raise SystemExit(1)

print("band (degC)      degC/s   seconds at full power")
for b in sorted(buckets):
    gained, secs = buckets[b]
    if secs < 4:  continue                      # too little evidence
    print("  %3.0f - %3.0f      %5.3f    %6.1f" % (b, b + band, gained / secs, secs))

hi = [(b, g / s) for b, (g, s) in sorted(buckets.items()) if s >= 4 and b >= 200]
if hi:
    print()
    print("above 200 degC: %.3f degC/s at %d, %.3f at %d"
          % (hi[0][1], hi[0][0], hi[-1][1], hi[-1][0]))
    slowest = min(r for _, r in hi)
    print("slowest band above 200: %.3f degC/s" % slowest)
    print("=> 200->245 needs at least %.0f s at full power" % (45.0 / slowest))
