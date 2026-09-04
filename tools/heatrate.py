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

# Two sources, two time units. ovenlog.sh mirrors /status, whose "time" is
# milliseconds; the firmware's own /history counts seconds. /history is the
# better source -- it cannot be starved by a second poller, which is how the
# 2026-09-04 loaded run lost 77 s straight through its fastest climb.
SECONDS_ALREADY = "dt" not in (rows[0] if rows else {})

pts = []
for r in rows:
    try:
        t = float(r["time"])
        if not SECONDS_ALREADY:
            t /= 1000.0
        pts.append((t, float(r["temp"]), float(r["power"])))
    except (ValueError, KeyError):
        continue

# The element lags the demand. A sample can read 100% duty while the element
# is still cold from a cut a moment earlier, which reads as a collapse in
# capability -- measured 2026-09-04, where a genuine 2.25 degC/s at 210-220
# was followed by an apparent 0.64 at 230-240 purely because the regulator had
# started cycling. So require the demand to have been full for a WHILE.
SETTLE = 3          # consecutive prior samples that must also be full power

buckets = defaultdict(lambda: [0.0, 0.0, 0])   # band -> [degC gained, seconds, samples]
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
    buckets[b][2] += 1

if not buckets:
    print("no full-power climbing samples found")
    raise SystemExit(1)

# Evidence is a COUNT OF SAMPLES, never a count of seconds. Seconds in a band
# fall as the rate rises, so a "secs < 4" filter deletes precisely the fast
# bands this tool exists to measure: at 1.7 degC/s a 5 degC band is crossed in
# 3 s and vanishes, while a sluggish band lingers and survives. That filter was
# here until 2026-09-04 and it silently truncated every report at the point the
# oven got quick -- a loaded run that reached 241 degC reported nothing above
# 70, and the surviving "slowest band above 200" was an artifact of the
# regulator cycling, not a capability.
MIN_SAMPLES = 3

print("band (degC)      degC/s   seconds   samples")
for b in sorted(buckets):
    gained, secs, n = buckets[b]
    if n < MIN_SAMPLES or secs <= 0:  continue
    print("  %3.0f - %3.0f      %5.3f    %6.1f    %4d"
          % (b, b + band, gained / secs, secs, n))

hi = [(b, g / s) for b, (g, s, n) in sorted(buckets.items())
      if n >= MIN_SAMPLES and s > 0 and b >= 200]
if hi:
    print()
    print("above 200 degC: %.3f degC/s at %d, %.3f at %d"
          % (hi[0][1], hi[0][0], hi[-1][1], hi[-1][0]))
    slowest = min(r for _, r in hi)
    print("slowest band above 200: %.3f degC/s" % slowest)
    print("=> 200->245 needs at least %.0f s at full power" % (45.0 / slowest))

# Ground truth, immune to how the bands happen to fall: walk the trace and time
# the actual climb between two temperatures at sustained full power. Binning is
# a summary; this is the measurement.
def crossing_time(lo, hi_t):
    """Time from lo to hi_t with the element at full power the WHOLE way.

    An earlier version latched t_lo and then returned at the first sample above
    hi_t without checking what happened in between, so a stretch of coasting
    counted as full-power capability. On the 2026-09-04 empty log that reported
    "200 -> 240 at full power: 68 s" for a climb the regulator had actually
    stopped driving at 209 -- the oven was accused of being slow while it was
    switched off.
    """
    t_lo = None
    run2 = 0
    for (t0, T0, P0), (t1, T1, _) in zip(pts, pts[1:]):
        settled = (run2 := run2 + 1 if P0 >= 95.0 else 0) > SETTLE
        if not settled:
            t_lo = None                 # power dropped: the clock restarts
            continue
        # Latch on an UPWARD CROSSING of lo, never on merely being above it.
        # Latching on ">= lo" restarts the clock wherever full power happens to
        # resume -- on the 2026-09-04 empty log that was 239 degC, and 200->240
        # was duly reported as 1 s at 35 degC/s.
        if t_lo is None and T0 < lo <= T1:  t_lo = t0
        if t_lo is not None and T1 >= hi_t:  return t1 - t_lo
    return None

print()
for lo, hi_t in ((200, 240), (200, 245), (150, 200), (100, 150)):
    el = crossing_time(lo, hi_t)
    if el:
        print("measured %d -> %d at full power: %.0f s  (%.2f degC/s)"
              % (lo, hi_t, el, (hi_t - lo) / el))
    else:
        print("measured %d -> %d: not reached at full power in this log" % (lo, hi_t))
