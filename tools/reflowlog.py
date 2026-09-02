#!/usr/bin/env python3
"""Log a reflow run from /status, and derive the oven's thermal lag from it.

Two modes:

    reflowlog.py log  [host] [-o run.csv]   poll /status to CSV until the run ends
    reflowlog.py lag  run.csv                derive thermal lag from a logged run

The lag is what the controller's overshoot compensation is built on:

    projected = temperature + rate * thermalLag

and it is measured by watching what the oven does when the power goes off:

    lag = (peak reached after the cut - temperature at the cut) / rate at the cut

Every heating step of a normal profile run performs that experiment, because
the approach clamp drops the power to zero as soon as arrival is assured. So a
single logged run yields one estimate per step, at different temperatures --
which is also how you see that the lag is not really one constant.

Prefer the estimate closest to your peak temperature. That is where an
overshoot damages parts, so that is the operating point the single number
should be honest about.

No dependencies beyond the standard library.
"""

import argparse
import csv
import json
import sys
import time
import urllib.request

FIELDS = ["time", "temp", "dt", "setpoint", "low", "high",
          "power", "step", "steps", "lag", "state", "fault"]

# Below this the "cut" is a rounding artefact rather than the power coming off.
POWER_OFF_PCT = 1.0
# A climb has to be real for the division to mean anything.
MIN_RATE_C_S = 0.05
# Ignore a coast that never went anywhere -- noise, not inertia.
MIN_COAST_C = 0.5


def poll(host, out, interval):
    url = "http://%s/status" % host
    writer = csv.DictWriter(out, fieldnames=["wall"] + FIELDS)
    writer.writeheader()
    seen_running = False
    while True:
        try:
            with urllib.request.urlopen(url, timeout=5) as r:
                d = json.load(r)
        except Exception as e:                      # oven rebooted, wifi blip
            print("poll failed: %s" % e, file=sys.stderr)
            time.sleep(interval)
            continue

        row = {k: d.get(k, "") for k in FIELDS}
        row["wall"] = "%.3f" % time.time()
        writer.writerow(row)
        out.flush()

        state = d.get("state", "")
        print("\r%-10s %6.1fC  %+5.2fC/s  %5.1f%%  step %s/%s   " % (
            state, d.get("temp", 0), d.get("dt", 0), d.get("power", 0),
            d.get("step", "-"), d.get("steps", "-")), end="", file=sys.stderr)

        if d.get("fault"):
            print("\nFAULT: %s" % d["fault"], file=sys.stderr)
            return
        if state in ("Running", "MeasureLag"):
            seen_running = True
        elif seen_running and state in ("Ready", "Complete"):
            print("\nrun finished (%s)" % state, file=sys.stderr)
            return
        time.sleep(interval)


def load(path):
    with open(path) as f:
        rows = []
        for r in csv.DictReader(f):
            try:
                rows.append({
                    "t": float(r["time"]) / 1000.0,
                    "temp": float(r["temp"]),
                    "dt": float(r["dt"]),
                    "power": float(r["power"]),
                    "step": r["step"],
                    "state": r["state"],
                })
            except (ValueError, KeyError):
                continue        # a blank row from a failed poll
    return rows


def find_cuts(rows):
    """Every falling edge of power to zero, with the coast that followed."""
    events = []
    for i in range(1, len(rows)):
        if not (rows[i - 1]["power"] > POWER_OFF_PCT
                and rows[i]["power"] <= POWER_OFF_PCT):
            continue
        cut = rows[i - 1]
        if cut["dt"] < MIN_RATE_C_S:
            continue            # not climbing: nothing to project

        # Follow the coast, and note *why* it ended. If the controller put
        # the power back on before the oven stopped climbing, the coast was
        # cut short and the lag it implies is only a lower bound. During a
        # normal profile run that is the common case -- the approach clamp
        # releases as soon as the projection drops back under the target -- so
        # these have to be told apart or the estimate reads systematically low.
        peak, peak_t = cut["temp"], cut["t"]
        complete = False
        for j in range(i, len(rows)):
            if rows[j]["power"] > POWER_OFF_PCT:
                break               # power resumed: truncated
            if rows[j]["temp"] > peak:
                peak, peak_t = rows[j]["temp"], rows[j]["t"]
            elif rows[j]["t"] - peak_t > 2.0:
                complete = True     # stopped climbing on its own: a real peak
                break
        # Falling out of the loop means the log ended mid-coast. That is not a
        # peak either -- the oven may well have kept climbing after the last
        # row -- so it stays a lower bound.
        coast = peak - cut["temp"]
        if coast < MIN_COAST_C:
            continue
        events.append({
            "t": cut["t"], "step": cut["step"],
            "cut_temp": cut["temp"], "rate": cut["dt"],
            "peak": peak, "coast": coast,
            "secs": peak_t - cut["t"],
            "lag": coast / cut["dt"],
            "complete": complete,
        })
    return events


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("log", help="poll /status to CSV until the run ends")
    p.add_argument("host", nargs="?", default="ReflowController.local")
    p.add_argument("-o", "--out", default="run.csv")
    p.add_argument("-i", "--interval", type=float, default=1.0)

    p = sub.add_parser("lag", help="derive thermal lag from a logged run")
    p.add_argument("csvfile")

    a = ap.parse_args()

    if a.cmd == "log":
        with open(a.out, "w", newline="") as f:
            poll(a.host, f, a.interval)
        print("wrote %s" % a.out, file=sys.stderr)
        return

    rows = load(a.csvfile)
    if not rows:
        sys.exit("no usable rows in %s" % a.csvfile)
    events = find_cuts(rows)
    if not events:
        sys.exit("no power-cut events found -- was the oven ever driven and "
                 "then cut while climbing?")

    print("%8s %5s %10s %10s %8s %8s %8s  %s" % (
        "t(s)", "step", "cut(C)", "rate(C/s)", "peak(C)", "coast(C)", "lag(s)",
        "coast"))
    for e in events:
        print("%8.0f %5s %10.1f %10.2f %8.1f %8.1f %8.1f  %s" % (
            e["t"], e["step"], e["cut_temp"], e["rate"],
            e["peak"], e["coast"], e["lag"],
            "full" if e["complete"] else "TRUNCATED (lower bound)"))

    good = [e for e in events if e["complete"]]
    print()
    if not good:
        print("Every coast here was truncated -- the controller put the power")
        print("back on before the oven stopped climbing, so all of these read")
        print("LOW and none of them is the number you want.")
        print()
        print("Run the dedicated measurement instead. It cuts the power and")
        print("lets the oven peak properly, which a profile run never does:")
        print()
        print("  curl 'http://<oven>:8080/measurelag'")
        return

    hottest = max(good, key=lambda e: e["cut_temp"])
    print("lag at the hottest full coast : %.1f s  (cut at %.0f C)"
          % (hottest["lag"], hottest["cut_temp"]))
    if len(good) > 1:
        cool = min(good, key=lambda e: e["cut_temp"])
        print("           ...at the coolest : %.1f s  (cut at %.0f C)"
              % (cool["lag"], cool["cut_temp"]))
        print()
        print("Expect the two to differ. The coast comes out of the element,")
        print("and the hotter the oven the smaller the element-to-air gap that")
        print("drives it, so the lag falls as the oven approaches its peak.")
    print()
    print("Take the hottest. Peak overshoot is what damages parts, so the one")
    print("constant should be honest at that end of the range; being long")
    print("lower down only costs a slightly slow ramp, which the step's")
    print("slow bound absorbs.")
    print()
    print("  curl 'http://<oven>:8080/oven?thermalLag=%.1f'" % hottest["lag"])


if __name__ == "__main__":
    main()
