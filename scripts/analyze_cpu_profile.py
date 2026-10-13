"""Summarize the process-only slicing sampler with log stages and CPU counters.

Running-state samples are estimates, not ETW on-CPU events. Inclusive function
weights overlap. Each batch is weighted by its sampling interval (capped at
100 ms to avoid assigning long symbol-loading gaps to a single stack).
"""

import argparse
import bisect
from collections import Counter
import csv
import datetime
import json
from pathlib import Path
import statistics


def log_boundaries(directory, metrics):
    start = datetime.datetime.fromisoformat(metrics["started"]).timestamp()
    result = [(start, "Loading and configuration")]
    markers = [
        ("Starting the slicing process.", "Mesh slicing"),
        ("Make overhang printable...", "Paint segmentation and Local-Z preparation"),
        ("Generating walls...", "Walls"),
        ("Detecting solid surfaces...", "Solid surface detection"),
        ("Discovering vertical shells...", "Vertical shells"),
        ("Processing external surfaces...", "External surfaces, bridges and infill"),
        ("Processing support tree mesh group", "Tree support"),
        ("Total time of organic tree support:", "Wipe tower and slicing finalization"),
        ("Exporting G-code...", "G-code export"),
        ("Exporting G-code finished", "Cleanup"),
    ]
    seen = set()
    for line in (directory / "slice" / "stdout.log").read_text(encoding="utf-8", errors="replace").splitlines():
        for marker, name in markers:
            if marker in line and name not in seen and line.startswith("["):
                result.append((datetime.datetime.fromisoformat(line[1:line.index("]")]).timestamp(), name))
                seen.add(name)
    result.append((start + metrics["wall_seconds"], "End"))
    return sorted(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--plot", action="store_true", help="Also save a CPU timeline PNG (requires matplotlib).")
    parser.add_argument("--label", default="Optimized Release build, process-only sampling run", help="Subtitle for the CPU timeline.")
    args = parser.parse_args()
    directory = args.directory
    metrics = json.loads((directory / "slice" / "metrics.json").read_text(encoding="utf-8"))
    symbols = {s["pc"]: s for s in map(json.loads, (directory / "profile_symbols.jsonl").read_text(encoding="utf-8").splitlines())}
    boundaries = log_boundaries(directory, metrics)
    times = [t for t, _ in boundaries]
    timeline = list(csv.DictReader((directory / "profile_timeline.csv").open(encoding="utf-8")))
    cpu_t = [times[0]] + [float(r["unix_time"]) for r in timeline] + [times[-1]]
    cpu_v = [0.] + [float(r["user_seconds"]) + float(r["kernel_seconds"]) for r in timeline] + [metrics["cpu_seconds"]]

    def cpu_at(t):
        i = min(max(bisect.bisect_right(cpu_t, t) - 1, 0), len(cpu_t) - 2)
        fraction = min(1., max(0., (t - cpu_t[i]) / max(1e-9, cpu_t[i + 1] - cpu_t[i])))
        return cpu_v[i] + fraction * (cpu_v[i + 1] - cpu_v[i])

    raw = list(csv.DictReader((directory / "profile_stacks.csv").open(encoding="utf-8")))
    batches = sorted(set(float(r["unix_time"]) for r in raw))
    intervals = [b - a for a, b in zip(batches, batches[1:])]
    median_interval = statistics.median(intervals)
    weights = {t: min(0.1, batches[i + 1] - t if i + 1 < len(batches) else median_interval) for i, t in enumerate(batches)}
    accumulators = {name: {"inclusive": Counter(), "exclusive": Counter(), "files": Counter(),
                           "threads": Counter(), "samples": 0, "weight": 0., "unresolved_leaf": 0,
                           "depth": Counter(), "states": Counter(), "call_sites": Counter(),
                           "wait_leaf_weight": 0., "source_leaf_weight": 0.} for _, name in boundaries[:-1]}
    accumulators["All"] = {"inclusive": Counter(), "exclusive": Counter(), "files": Counter(),
                           "threads": Counter(), "samples": 0, "weight": 0., "unresolved_leaf": 0,
                           "depth": Counter(), "states": Counter(), "call_sites": Counter(),
                           "wait_leaf_weight": 0., "source_leaf_weight": 0.}
    folded = Counter()
    for row in raw:
        t = float(row["unix_time"])
        stage = boundaries[min(max(0, bisect.bisect_right(times, t) - 1), len(boundaries) - 2)][1]
        state = int(row["state"])
        for name in (stage, "All"):
            accumulators[name]["states"][state] += 1
        if state != 2:
            continue
        frames = [symbols.get(pc, {"name": "[unresolved]", "module": "", "file": "", "line": 0})
                  for pc in row["addresses_leaf_first"].split(";") if pc]
        if not frames:
            continue
        names = [f'{s["module"]}!{s["name"]}' for s in frames]
        weight = weights[t]
        folded[";".join(reversed([n.replace(";", ",") for n in names]))] += weight
        for name in (stage, "All"):
            acc = accumulators[name]
            acc["samples"] += 1
            acc["weight"] += weight
            acc["depth"][len(frames)] += 1
            acc["threads"][row["tid"]] += weight
            acc["unresolved_leaf"] += frames[0]["name"] == "[unresolved]"
            if any(token in frames[0]["name"] for token in ("WaitFor", "NtWait", "ZwWait", "NtDelay", "ZwDelay", "SwitchToThread")):
                acc["wait_leaf_weight"] += weight
            if frames[0]["file"]:
                acc["source_leaf_weight"] += weight
            acc["exclusive"][names[0]] += weight
            acc["files"][frames[0]["file"] or frames[0]["module"]] += weight
            for function in set(names):
                acc["inclusive"][function] += weight
            for frame in frames:
                if frame["name"] == "Slic3r::GCode::process_layer":
                    acc["call_sites"][f'{frame["file"]}:{frame["line"]}'] += weight
    result = {"method": __doc__, "metrics": {k: v for k, v in metrics.items() if k != "samples"},
              "batch_count": len(batches), "batch_interval_median_ms": median_interval * 1000,
              "batch_interval_max_ms": max(intervals) * 1000,
              "intervals_above_100ms": sum(d > .1 for d in intervals), "stages": []}

    def summarize(acc):
        def leaders(counter):
            return [{"name": name, "weight": weight, "percent": 100. * weight / acc["weight"]}
                    for name, weight in counter.most_common(80)] if acc["weight"] else []
        return {"running_samples": acc["samples"], "running_sample_weight": acc["weight"],
                "unresolved_leaf_samples": acc["unresolved_leaf"], "states": dict(acc["states"]),
                "depth_histogram": dict(acc["depth"]),
                "wait_leaf_percent": 100. * acc["wait_leaf_weight"] / acc["weight"] if acc["weight"] else 0.,
                "source_leaf_percent": 100. * acc["source_leaf_weight"] / acc["weight"] if acc["weight"] else 0.,
                **{k: leaders(acc[k]) for k in ("inclusive", "exclusive", "files", "threads", "call_sites")}}

    for i, (t, name) in enumerate(boundaries[:-1]):
        end = times[i + 1]
        wall = end - t
        cpu = cpu_at(end) - cpu_at(t)
        result["stages"].append({"name": name, "start": t, "end": end, "wall_seconds": wall,
                                 "cpu_seconds": cpu, "average_busy_cores": cpu / wall,
                                 **summarize(accumulators[name])})
    result["all"] = summarize(accumulators["All"])
    (directory / "analysis.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    with (directory / "stacks.folded").open("w", encoding="utf-8") as stream:
        for stack, weight in folded.items():
            stream.write(f"{stack} {round(weight * 1000000)}\n")
    for stage in result["stages"]:
        print(f'{stage["name"]}: wall {stage["wall_seconds"]:.2f}s, CPU {stage["cpu_seconds"]:.2f}s, '
              f'busy cores {stage["average_busy_cores"]:.2f}, samples {stage["running_samples"]}')
    print("Top inclusive functions (overlap):")
    for row in result["all"]["inclusive"][:25]:
        print(f'{row["percent"]:6.2f}% {row["name"]}')
    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        figure, axes = plt.subplots(2, 1, figsize=(12, 8), gridspec_kw={"height_ratios": [1, 1.4]})
        step = 2.
        x = [times[0] + i * step for i in range(int((times[-1] - times[0]) / step) + 1)]
        y = [(cpu_at(b) - cpu_at(a)) / (b - a) for a, b in zip(x, x[1:])]
        axes[0].step([t - times[0] for t in x[:-1]], y, where="post", color="#007f86", linewidth=1.5)
        export = next(s for s in result["stages"] if s["name"] == "G-code export")
        axes[0].axvspan(export["start"] - times[0], export["end"] - times[0], color="#d49936", alpha=.15,
                       label="G-code export")
        axes[0].set(xlabel="Elapsed seconds", ylabel="Busy CPU cores (2 s average)", ylim=(0, metrics["logical_processors"]))
        axes[0].legend(loc="upper right")
        axes[0].grid(axis="y", alpha=.2)
        stages = result["stages"]
        bars = axes[1].barh([s["name"] for s in stages], [s["wall_seconds"] for s in stages],
                            color=["#d49936" if s["name"] == "G-code export" else "#007f86" for s in stages])
        axes[1].invert_yaxis()
        axes[1].bar_label(bars, labels=[f'{s["wall_seconds"]:.1f} s' for s in stages], padding=4)
        axes[1].set(xlabel="Elapsed seconds", xlim=(0, max(s["wall_seconds"] for s in stages) * 1.15))
        axes[1].grid(axis="x", alpha=.2)
        figure.suptitle("Starry Night slicing: measured CPU activity and stage duration\n" + args.label)
        figure.tight_layout()
        figure.savefig(directory / "cpu_timeline.png", dpi=150)
        plt.close(figure)


if __name__ == "__main__":
    main()
