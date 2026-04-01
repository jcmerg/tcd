#!/usr/bin/env python3
"""
agc-analyze.py — Analyze tcd stats CSV files for AGC tuning

Usage:
  agc-analyze /tmp/tcd-stats/              # detail view of all files
  agc-analyze -s /tmp/tcd-stats/           # summary table
  agc-analyze file.csv                     # single file detail
  agc-analyze -s /tmp/tcd-stats/ -c dmr    # filter by codec
  agc-analyze -s /tmp/tcd-stats/ -m F      # filter by module

Default statsdir: /tmp/tcd-stats
"""

import csv, sys, os, glob, argparse
from collections import defaultdict

TARGET = -16  # default AGC target in dBFS


def analyze_file(path):
    """Analyze a single CSV stats file, return dict of metrics."""
    with open(path) as f:
        rows = list(csv.DictReader(f))

    if not rows:
        return None

    total = len(rows)
    duration = total * 0.02  # 20ms per frame

    module = rows[0]["module"]
    codec = rows[0]["codec"]

    # Classify frames
    speech, gated, silent = [], [], []
    for r in rows:
        rms_in = float(r["rms_in"])
        gate = int(r["gate"])
        if rms_in < -80:
            silent.append(r)
        elif gate:
            gated.append(r)
        else:
            speech.append(r)

    result = {
        "file": os.path.basename(path),
        "module": module,
        "codec": codec,
        "duration": duration,
        "total_frames": total,
        "speech_frames": len(speech),
        "gated_frames": len(gated),
        "silent_frames": len(silent),
        "speech_pct": len(speech) / total * 100 if total else 0,
    }

    if not speech:
        result.update({
            "rms_in_min": -100, "rms_in_avg": -100, "rms_in_max": -100,
            "rms_out_min": -100, "rms_out_avg": -100, "rms_out_max": -100,
            "peak_in_max": -100, "peak_out_max": -100,
            "gain_min": 0, "gain_avg": 0, "gain_max": 0,
            "target_dev": 0, "gain_std": 0, "gate_transitions": 0,
            "clipping": 0, "gain_ceiling_pct": 0, "gain_floor_pct": 0,
        })
        return result

    rms_in  = [float(r["rms_in"]) for r in speech]
    rms_out = [float(r["rms_out"]) for r in speech]
    peak_in = [float(r["peak_in"]) for r in speech]
    peak_out = [float(r["peak_out"]) for r in speech]
    gains   = [float(r["agc_gain"]) for r in speech]

    avg_out = sum(rms_out) / len(rms_out)
    avg_g = sum(gains) / len(gains)

    # How often is gain at ceiling (within 1dB of max)
    gain_ceiling = sum(1 for g in gains if g > max(gains) - 1.0)
    gain_floor = sum(1 for g in gains if g < min(gains) + 1.0)

    result.update({
        "rms_in_min": min(rms_in),
        "rms_in_avg": sum(rms_in) / len(rms_in),
        "rms_in_max": max(rms_in),
        "rms_out_min": min(rms_out),
        "rms_out_avg": avg_out,
        "rms_out_max": max(rms_out),
        "peak_in_max": max(peak_in),
        "peak_out_max": max(peak_out),
        "gain_min": min(gains),
        "gain_avg": avg_g,
        "gain_max": max(gains),
        "target_dev": avg_out - TARGET,  # positive = too loud, negative = too quiet
        "gain_std": (sum((g - avg_g)**2 for g in gains) / len(gains)) ** 0.5,
        "clipping": sum(1 for p in peak_out if p > -1.0),
        "gain_ceiling_pct": gain_ceiling / len(gains) * 100,
        "gain_floor_pct": gain_floor / len(gains) * 100,
    })

    # Gate flicker: count gate→speech transitions
    transitions = 0
    prev_gate = False
    for r in rows:
        g = int(r["gate"]) == 1
        if prev_gate and not g:
            transitions += 1
        prev_gate = g
    result["gate_transitions"] = transitions

    return result


def diagnose(r):
    """Return list of diagnostic messages with severity (info/warn/fix)."""
    if r["speech_frames"] == 0:
        return [("info", "No speech detected")]

    msgs = []
    dev = r["target_dev"]  # negative = too quiet
    ceil_pct = r["gain_ceiling_pct"]
    floor_pct = r["gain_floor_pct"]
    gstd = r["gain_std"]
    gtrans = r["gate_transitions"]
    dur = r["duration"]
    gmax = r["gain_max"]
    clip = r["clipping"]

    # Case 1: Output too quiet because gain ceiling reached
    if dev < -4 and ceil_pct > 30:
        needed = abs(dev) + gmax
        msgs.append(("fix",
            f"Output {abs(dev):.0f}dB too quiet, AGC at ceiling {ceil_pct:.0f}% of the time. "
            f"Input needs {needed:.0f}dB total gain. "
            f"-> Increase AGCMaxGainUp to {needed + 3:.0f} (currently max {gmax:.0f}dB)"))

    # Case 2: Output too quiet but gain NOT at ceiling — target too low or release too slow
    elif dev < -4 and ceil_pct < 10:
        msgs.append(("fix",
            f"Output {abs(dev):.0f}dB too quiet, but AGC has headroom (ceiling only {ceil_pct:.0f}%). "
            f"-> Either raise AGCTarget to {TARGET + round(abs(dev)):.0f} dBFS, "
            f"or decrease AGCRelease for faster tracking"))

    # Case 3: Output too loud
    elif dev > 4:
        msgs.append(("fix",
            f"Output {dev:.0f}dB too loud. "
            f"-> Lower AGCTarget to {TARGET - round(dev):.0f} dBFS, "
            f"or increase AGCMaxGainDown"))

    # Case 4: Moderate deviation
    elif abs(dev) > 2:
        direction = "quiet" if dev < 0 else "loud"
        msgs.append(("warn", f"Output {abs(dev):.1f}dB too {direction} (minor)"))

    # Case 5: Output good
    else:
        msgs.append(("info", f"Output level good (deviation {abs(dev):.1f}dB from target)"))

    # Gain stability
    if gstd > 4:
        msgs.append(("fix",
            f"AGC pumping: gain varies {gstd:.1f}dB std. "
            f"-> Increase AGCRelease (slower recovery) or increase AGCAttack"))
    elif gstd > 2.5:
        msgs.append(("warn", f"Gain somewhat unstable ({gstd:.1f}dB std)"))

    # Gate flicker
    if dur > 5 and gtrans / dur > 1.5:
        msgs.append(("fix",
            f"Noise gate flicker: {gtrans} transitions in {dur:.0f}s ({gtrans/dur:.1f}/s). "
            f"-> Lower AGCNoiseGate (e.g. -60 dBFS)"))
    elif dur > 5 and gtrans / dur > 0.8:
        msgs.append(("warn",
            f"Moderate gate activity: {gtrans} transitions in {dur:.0f}s"))

    # Clipping
    if clip > 0:
        msgs.append(("fix",
            f"{clip} near-clip frames (peak > -1 dBFS). "
            f"-> Lower AGCTarget or increase AGCMaxGainDown"))

    # Gain ceiling persistent
    if ceil_pct > 80:
        msgs.append(("warn",
            f"AGC at ceiling {ceil_pct:.0f}% of speech — very quiet input ({r['rms_in_avg']:.0f} dBFS avg)"))

    return msgs


def print_detail(r):
    """Print detailed analysis of one file."""
    print(f"\n{'='*60}")
    print(f"File:     {r['file']}")
    print(f"Module:   {r['module']}  Codec: {r['codec']}")
    print(f"Duration: {r['duration']:.1f}s  ({r['total_frames']} frames)")
    print(f"Speech:   {r['speech_frames']} ({r['speech_pct']:.0f}%)  "
          f"Gated: {r['gated_frames']}  Silent: {r['silent_frames']}")

    if r["speech_frames"] == 0:
        print("  (no speech detected)")
        return

    print(f"\n  {'':12s} {'Min':>8s} {'Avg':>8s} {'Max':>8s}")
    print(f"  {'RMS In':12s} {r['rms_in_min']:>7.1f}  {r['rms_in_avg']:>7.1f}  {r['rms_in_max']:>7.1f}  dBFS")
    print(f"  {'RMS Out':12s} {r['rms_out_min']:>7.1f}  {r['rms_out_avg']:>7.1f}  {r['rms_out_max']:>7.1f}  dBFS")
    print(f"  {'AGC Gain':12s} {r['gain_min']:>+7.1f}  {r['gain_avg']:>+7.1f}  {r['gain_max']:>+7.1f}  dB")

    print(f"\n  Target ({TARGET} dBFS) deviation: {r['target_dev']:+.1f} dB")
    print(f"  Gain stability:   {r['gain_std']:.1f} dB std dev")
    print(f"  Gain at ceiling:  {r['gain_ceiling_pct']:.0f}% of speech frames")
    print(f"  Gate transitions: {r['gate_transitions']}")
    print(f"  Near-clip frames: {r['clipping']}")

    msgs = diagnose(r)
    if msgs:
        print(f"\n  Diagnosis:")
        for sev, msg in msgs:
            icon = {"info": "  ", "warn": "  !", "fix": "  >>"}[sev]
            print(f"  {icon} {msg}")


def print_summary(results):
    """Print one-line-per-file summary table."""
    print(f"\n{'File':<40s} {'Mod':>3s} {'Codec':>7s} {'Dur':>5s} {'Spch%':>5s} "
          f"{'In avg':>7s} {'Out avg':>7s} {'Gain':>6s} {'Dev':>5s} {'Ceil%':>5s} {'Stab':>4s} {'Gate':>4s}")
    print("-" * 110)
    for r in results:
        if r["speech_frames"] == 0:
            print(f"{r['file']:<40s} {r['module']:>3s} {r['codec']:>7s} {r['duration']:>4.0f}s {'0%':>5s}  (silent)")
            continue
        print(f"{r['file']:<40s} {r['module']:>3s} {r['codec']:>7s} {r['duration']:>4.0f}s "
              f"{r['speech_pct']:>4.0f}% {r['rms_in_avg']:>6.1f}  {r['rms_out_avg']:>6.1f}  "
              f"{r['gain_avg']:>+5.1f} {r['target_dev']:>+5.1f} {r['gain_ceiling_pct']:>4.0f}% "
              f"{r['gain_std']:>4.1f} {r['gate_transitions']:>4d}")

    with_speech = [r for r in results if r["speech_frames"] > 0]
    if not with_speech:
        return

    # Aggregate
    print("-" * 110)
    n = len(with_speech)
    total_dur = sum(r["duration"] for r in with_speech)
    avg_in = sum(r["rms_in_avg"] for r in with_speech) / n
    avg_out = sum(r["rms_out_avg"] for r in with_speech) / n
    avg_gain = sum(r["gain_avg"] for r in with_speech) / n
    avg_dev = sum(r["target_dev"] for r in with_speech) / n
    avg_ceil = sum(r["gain_ceiling_pct"] for r in with_speech) / n
    avg_stab = sum(r["gain_std"] for r in with_speech) / n
    total_gate = sum(r["gate_transitions"] for r in with_speech)

    print(f"{'AVERAGE (' + str(n) + ' streams)':<40s} {'':>3s} {'':>7s} {total_dur:>4.0f}s "
          f"{'':>5s} {avg_in:>6.1f}  {avg_out:>6.1f}  "
          f"{avg_gain:>+5.1f} {avg_dev:>+5.1f} {avg_ceil:>4.0f}% "
          f"{avg_stab:>4.1f} {total_gate:>4d}")

    # Overall diagnosis
    print(f"\n{'='*60}")
    print(f"Overall Assessment ({n} streams, {total_dur:.0f}s total):")
    print(f"  Avg input:  {avg_in:.1f} dBFS")
    print(f"  Avg output: {avg_out:.1f} dBFS (target: {TARGET})")
    print(f"  Avg gain:   {avg_gain:+.1f} dB")
    print()

    issues = []

    # Check if gain ceiling is the bottleneck
    ceiling_streams = [r for r in with_speech if r["gain_ceiling_pct"] > 30]
    if ceiling_streams:
        max_needed = max(abs(r["target_dev"]) + r["gain_max"] for r in ceiling_streams)
        recommended = min(max_needed + 3, 30)  # cap at 30dB — beyond that noise dominates
        if max_needed > 30:
            issues.append((
                f"{len(ceiling_streams)}/{n} streams hit gain ceiling (>30% of speech). "
                f"Quietest input needs {max_needed:.0f}dB total gain, but >30dB amplifies noise. "
                f"These are likely sender-side mic gain problems.",
                f"-> Set AGCMaxGainUp = {recommended:.0f} (practical limit, won't fix extremely quiet input)"
            ))
        else:
            issues.append((
                f"{len(ceiling_streams)}/{n} streams hit gain ceiling (>30% of speech). "
                f"Quietest input needs {max_needed:.0f}dB total gain.",
                f"-> Set AGCMaxGainUp = {recommended:.0f}"
            ))

    # Check if output is systematically off-target without ceiling
    no_ceil = [r for r in with_speech if r["gain_ceiling_pct"] < 10]
    if no_ceil:
        avg_dev_noceil = sum(r["target_dev"] for r in no_ceil) / len(no_ceil)
        if avg_dev_noceil < -3:
            # Output too quiet despite headroom — AGC tracks too slowly
            # Compensate by raising the target (louder aim compensates dynamic undershoot)
            compensated = TARGET - round(avg_dev_noceil)  # subtract negative = raise
            issues.append((
                f"{len(no_ceil)}/{n} streams with headroom are {abs(avg_dev_noceil):.0f}dB too quiet. "
                f"AGC undershoots due to speech dynamics and release time.",
                f"-> Raise AGCTarget = {compensated} (compensate {abs(avg_dev_noceil):.0f}dB undershoot) "
                f"or decrease AGCRelease (faster tracking)"
            ))
        elif avg_dev_noceil > 3:
            issues.append((
                f"{len(no_ceil)}/{n} streams with headroom are {avg_dev_noceil:.0f}dB too loud.",
                f"-> Lower AGCTarget = {TARGET - round(avg_dev_noceil)}"
            ))

    # Check gate flicker
    flickery = [r for r in with_speech if r["duration"] > 5 and r["gate_transitions"] / r["duration"] > 1.5]
    if flickery:
        issues.append((
            f"{len(flickery)}/{n} streams have excessive gate flicker.",
            f"-> Lower AGCNoiseGate (e.g. -60)"
        ))

    # Check pumping
    pumpy = [r for r in with_speech if r["gain_std"] > 4]
    if pumpy:
        issues.append((
            f"{len(pumpy)}/{n} streams show AGC pumping (gain std > 4dB).",
            f"-> Increase AGCRelease (e.g. 800ms)"
        ))

    # Check clipping
    clippy = [r for r in with_speech if r["clipping"] > 0]
    if clippy:
        total_clips = sum(r["clipping"] for r in clippy)
        issues.append((
            f"{len(clippy)}/{n} streams have near-clipping ({total_clips} frames total).",
            f"-> Lower AGCTarget or increase AGCMaxGainDown"
        ))

    if issues:
        print("  Issues found:")
        for desc, fix in issues:
            print(f"    {desc}")
            print(f"      {fix}")
            print()
    else:
        print("  No issues found — AGC is well-tuned!")

    # Print recommended config
    if issues:
        print("  Suggested tcd.ini changes:")
        for _, fix in issues:
            # Extract key=value pairs from fix string
            import re
            for m in re.finditer(r'(AGC\w+)\s*=\s*(-?\d+)', fix):
                print(f"    {m.group(1)} = {m.group(2)}")


def main():
    parser = argparse.ArgumentParser(description="Analyze tcd AGC stats CSV files")
    parser.add_argument("path", nargs="?", default="/tmp/tcd-stats",
                        help="Stats directory or single CSV file")
    parser.add_argument("--summary", "-s", action="store_true",
                        help="One-line summary per file")
    parser.add_argument("--codec", "-c", help="Filter by codec (e.g. dmr, dstar)")
    parser.add_argument("--module", "-m", help="Filter by module letter (e.g. F, S)")
    parser.add_argument("--target", "-t", type=float, default=-16,
                        help="AGC target in dBFS (default: -16)")
    args = parser.parse_args()

    global TARGET
    TARGET = args.target

    if os.path.isfile(args.path):
        files = [args.path]
    elif os.path.isdir(args.path):
        files = sorted(glob.glob(os.path.join(args.path, "*.csv")))
    else:
        print(f"Not found: {args.path}")
        sys.exit(1)

    if not files:
        print(f"No CSV files in {args.path}")
        sys.exit(0)

    results = []
    for f in files:
        r = analyze_file(f)
        if r is None:
            continue
        if args.codec and args.codec.lower() not in r["codec"].lower():
            continue
        if args.module and r["module"] != args.module.upper():
            continue
        results.append(r)

    if not results:
        print("No matching data")
        sys.exit(0)

    if args.summary:
        print_summary(results)
    else:
        for r in results:
            print_detail(r)
        if len(results) > 1:
            print_summary(results)


if __name__ == "__main__":
    main()
