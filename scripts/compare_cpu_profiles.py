"""Compare analyzed Local-Z-active and Local-Z-disabled sampling runs.

Percentages of CPU samples are approximate and inclusive functions overlap.
Turning off Local-Z/gradients changes the output workload; this is not a test
of equivalent-output optimization. Run analyze_cpu_profile.py on both first.
"""

import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import statistics


HOT_FUNCTIONS = (
    "clip_extrusion_collection_for_local_z", "append_clipped_path",
    "extrusion_collection_has_local_z_segment", "collect_local_z_wipe_tower_toolchanges",
    "local_z_compensate_masks", "SeamPlacer::init", "SeamPlacer::align_seam_points",
    "GlobalModelInfo::calculate_point_visibility", "GCodeProcessor::process_buffer",
    "GCodeProcessor::process_G1", "GCodeProcessor::finalize", "GCode::_extrude", "GCode::process_layer",
)


def read_profile(directory):
    analysis = json.loads((directory / "analysis.json").read_text(encoding="utf-8"))
    metrics = analysis["metrics"]
    if metrics["exit_code"] != 0 or metrics["timed_out"] or not metrics["gcodes"]:
        raise ValueError(f"Incomplete slice: {directory}")
    symbols = {s["pc"]: s for s in map(json.loads, (directory / "profile_symbols.jsonl").read_text(encoding="utf-8").splitlines())}
    rows = list(csv.DictReader((directory / "profile_stacks.csv").open(encoding="utf-8")))
    batches = sorted({float(row["unix_time"]) for row in rows})
    median = statistics.median(b - a for a, b in zip(batches, batches[1:]))
    weights = {t: min(.1, batches[i + 1] - t if i + 1 < len(batches) else median) for i, t in enumerate(batches)}
    export = next(s for s in analysis["stages"] if s["name"] == "G-code export")
    hits = {scope: Counter() for scope in ("all", "export")}
    totals = Counter()
    for row in rows:
        if row["state"] != "2":
            continue
        timestamp = float(row["unix_time"])
        names = {symbols[pc]["name"] for pc in row["addresses_leaf_first"].split(";") if pc}
        scopes = ["all"] + (["export"] if export["start"] <= timestamp < export["end"] else [])
        for scope in scopes:
            totals[scope] += weights[timestamp]
            for function in HOT_FUNCTIONS:
                # Match the end of a fully qualified function, not names of
                # templates/lambdas that happen to mention a parent function.
                if any(name.endswith("::" + function) for name in names):
                    hits[scope][function] += weights[timestamp]
    log = (directory / "slice" / "stdout.log").read_text(encoding="utf-8", errors="replace")
    markers = ("Local-Z plan built", "Local-Z phase-b prepared", "Local-Z phase-b emitting",
               "Local-Z wipe tower dependency scheduler", "preplanned wipe tower sequence mismatch",
               "toolchange reserve exhausted", "trailing wipe tower transition cannot be reconciled", "base clip leak")
    analysis["selected_functions"] = {scope: {name: 100. * counts[name] / totals[scope] for name in HOT_FUNCTIONS}
                                       for scope, counts in hits.items()}
    analysis["log_markers"] = {marker: log.count(marker) for marker in markers}
    return analysis


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--active", type=Path, required=True)
    parser.add_argument("--disabled", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--plot", action="store_true")
    args = parser.parse_args()
    active, disabled = read_profile(args.active), read_profile(args.disabled)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    assert active["metrics"]["dll_sha256"] == disabled["metrics"]["dll_sha256"]
    assert active["metrics"]["project_sha256"] == manifest["source_sha256"]
    assert disabled["metrics"]["project_sha256"] == manifest["output_sha256"]
    assert active["log_markers"]["Local-Z plan built"] > 0
    assert not any(disabled["log_markers"].values()), disabled["log_markers"]
    for function in HOT_FUNCTIONS[:5]:
        assert disabled["selected_functions"]["all"][function] == 0, function
    stages = []
    disabled_stages = {s["name"]: s for s in disabled["stages"]}
    for before in active["stages"]:
        after = disabled_stages[before["name"]]
        stages.append({"name": before["name"], "active_seconds": before["wall_seconds"],
                       "disabled_seconds": after["wall_seconds"],
                       "saved_seconds": before["wall_seconds"] - after["wall_seconds"],
                       "active_busy_cores": before["average_busy_cores"], "disabled_busy_cores": after["average_busy_cores"]})
    before, after = active["metrics"]["wall_seconds"], disabled["metrics"]["wall_seconds"]
    result = {"method": __doc__, "manifest": manifest, "active": active, "disabled": disabled, "stages": stages,
              "total": {"active_seconds": before, "disabled_seconds": after, "saved_seconds": before - after,
                        "reduction_percent": 100. * (before - after) / before, "speed_ratio": before / after}}
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "comparison.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result["total"], indent=2))
    for row in stages:
        print(f'{row["name"]}: {row["active_seconds"]:.2f} -> {row["disabled_seconds"]:.2f} seconds')
    print("Export function sample shares:")
    for name in HOT_FUNCTIONS:
        print(f'{name}: {active["selected_functions"]["export"][name]:.2f}% -> '
              f'{disabled["selected_functions"]["export"][name]:.2f}%')
    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        labels = [r["name"].replace("Paint segmentation and Local-Z preparation", "Paint / slicing preparation") for r in stages]
        labels += ["Total"]
        a = [r["active_seconds"] for r in stages] + [before]
        b = [r["disabled_seconds"] for r in stages] + [after]
        figure, axes = plt.subplots(figsize=(12, 9))
        positions = list(range(len(labels)))
        bars_a = axes.barh([v - .18 for v in positions], a, height=.34, color="#b46622", label="Local-Z active (gradient recipes)")
        bars_b = axes.barh([v + .18 for v in positions], b, height=.34, color="#007f86", label="Local-Z / gradients disabled")
        axes.bar_label(bars_a, labels=[f'{v:.1f}' for v in a], padding=3, fontsize=9)
        axes.bar_label(bars_b, labels=[f'{v:.1f}' for v in b], padding=3, fontsize=9)
        axes.set_yticks(positions, labels)
        axes.invert_yaxis()
        axes.set(xlabel="Elapsed seconds", xlim=(0, max(a + b) * 1.15),
                 title="Starry Night: cost of Local-Z on the same Release build\nSame model and paint assignments; three recipe gradient flags disabled")
        axes.legend(loc="lower right")
        axes.grid(axis="x", alpha=.2)
        figure.tight_layout()
        figure.savefig(args.output / "local_z_comparison.png", dpi=150)
        plt.close(figure)


if __name__ == "__main__":
    main()
