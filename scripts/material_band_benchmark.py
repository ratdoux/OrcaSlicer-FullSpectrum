#!/usr/bin/env python3
"""Release M7 benchmarks: cold processes, cache hits, edits, paired feature-off captures."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import statistics
import subprocess
import time
from material_band_baseline import capture, digest, peak_working_set, CASES

def suite():
    cases = []
    def add(name, **spec):
        cases.append(dict(id=name, **spec))
    add("P1-slab", shape="MB01", height=.48, expected="")
    add("P1-transition", shape="MB02-transition", height=.48, expected="")
    add("P1-cylinder", shape="cylinder", height=.48, resolution=64, expected="")
    for regions in (1, 4, 16, 64, 256):
        add(f"P2-regions-{regions}", shape="stress", regions=regions, recipes=min(2, regions), physical=2,
            topology="stripes", height_change="constant", resolution=0, process="classic", state="translation", threads=2,
            seed=20261026, expected="paint_domain_candidate_limit" if regions == 256 else "paint_not_matching_full_height_stripes")
    for resolution in (16, 32, 64):
        add(f"P3-mesh-{resolution}", shape="cylinder", height=.48, resolution=resolution, expected="")
    for cycles in (1, 2, 4, 8):
        add(f"P4-cycles-{cycles}", shape="MB01", height=.24*cycles, expected="")
    for copies in (1, 2, 4, 8):
        add(f"P5-hole-copies-{copies}", shape="hole_plate", height=.48, resolution=64, copies=copies,
            infill_pattern="concentric", infill_angle=0., expected="")
    add("P5-taper", shape="frustum", height=.48, resolution=64, expected="")
    add("P6-edit-revert", shape="MB02-transition", height=.48, edit_revert=True, expected="")
    for material_count in (3, 4):
        for gradient in (False, True):
            add(f"P7-{material_count}-{'gradient' if gradient else 'blend'}", material_count=material_count,
                gradient=gradient, expected="")
    return cases

def summary(samples):
    common = set.intersection(*(set(s) for s in samples))
    return {key: {"median": statistics.median(s[key] for s in samples), "min": min(s[key] for s in samples),
                  "max": max(s[key] for s in samples)} for key in sorted(common)
            if all(isinstance(s[key], (int, float)) and not isinstance(s[key], bool) for s in samples)}

def assess(report):
    # Gross regression budgets for these named numerical profiles, established
    # after the slab/clipping stages. They are not universal model limits.
    limits = {"snapshot_ms": 500, "paths_ms": 8000, "finalize_ms": 20000,
              "export_ms": 1000, "peak_working_set_bytes": 256 * 1024 * 1024,
              "cache_hit_ms": 10}
    failures = []
    for name, case in report["enabled"].items():
        for metric, limit in limits.items():
            if metric in case["summary"] and case["summary"][metric]["median"] > limit:
                failures.append(f"{name}: median {metric} exceeds {limit}")
        if name.startswith("P5-hole-copies-"):
            if case["summary"]["geometry_reuses"]["median"] != case["spec"]["copies"] - 1:
                failures.append(f"{name}: immutable copy geometry was not reused")
    first, last = report["enabled"].get("P5-hole-copies-1"), report["enabled"].get("P5-hole-copies-8")
    if first and last and last["summary"]["candidates"]["median"] > 12 * first["summary"]["candidates"]["median"]:
        failures.append("P5: candidate growth exceeds 12x for 8x fixed-neighborhood copies")
    off = {name: {"time_pass": case.get("paired_time_ratio", case["time_ratio"]) <= 1.05,
                  "memory_pass": case["memory_ratio"] <= 1.05}
           for name, case in report["feature_off"].items()}
    return {"enabled_median_limits": limits, "enabled_failures": failures, "enabled_pass": not failures,
            "feature_off_time_estimator": "median(new_time_i / old_time_i) for adjacent paired captures",
            "feature_off_5_percent": off,
            "feature_off_ratio_of_medians_pass": bool(off) and all(c["time_ratio"] <= 1.05 for c in report["feature_off"].values()),
            "feature_off_pass": bool(off) and all(v["time_pass"] and v["memory_pass"] for v in off.values())}

def benchmark(executable, directory, spec):
    directory.mkdir(parents=True, exist_ok=True)
    source, destination = directory / "spec.json", directory / "metrics.json"
    source.write_text(json.dumps(spec, indent=2) + "\n")
    env = dict(os.environ, MATERIAL_BAND_BENCHMARK_INPUT=str(source.resolve()),
               MATERIAL_BAND_BENCHMARK_OUTPUT=str(destination.resolve()))
    started = time.perf_counter()
    with (directory / "test.log").open("wb") as log:
        test_filter = "[material_band][stack_benchmark]" if "material_count" in spec else "[material_band][benchmark]~[stack_benchmark]"
        process = subprocess.Popen([str(executable), test_filter, "--rng-seed", "20261026"],
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=300)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
            raise RuntimeError(f"{spec['id']} exceeded 300 s; bundle: {directory}") from None
        peak = peak_working_set(process)
    if code:
        raise RuntimeError(f"{spec['id']} failed; bundle: {directory}")
    metrics = json.loads(destination.read_text())
    metrics.update(peak_working_set_bytes=peak, process_wall_ms=(time.perf_counter()-started)*1000)
    destination.write_text(json.dumps(metrics, indent=2)+"\n")
    return metrics

def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--test-exe", type=Path, required=True)
    parser.add_argument("--baseline-exe", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--only", help="Benchmark ID prefix")
    parser.add_argument("--resume", action="store_true", help="Reuse completed cases from the same binaries and run count")
    parser.add_argument("--cpu-affinity", type=lambda value: int(value, 0), help="Windows CPU mask inherited by captures, e.g. 0x3")
    args = parser.parse_args()
    if args.runs < 5:
        parser.error("At least five paired measurements are required")
    if args.cpu_affinity is not None:
        if os.name != "nt" or args.cpu_affinity <= 0:
            parser.error("--cpu-affinity requires Windows and a positive processor mask")
        from ctypes import wintypes
        current_process = ctypes.windll.kernel32.GetCurrentProcess
        current_process.restype = wintypes.HANDLE
        set_affinity = ctypes.windll.kernel32.SetProcessAffinityMask
        set_affinity.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        set_affinity.restype = wintypes.BOOL
        if not set_affinity(current_process(), args.cpu_affinity):
            raise ctypes.WinError()
    exe, output = args.test_exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"binary_sha256": digest(exe.read_bytes()), "threads": 2, "warmups": 1, "measured_runs": args.runs,
              "cpu_affinity": hex(args.cpu_affinity) if args.cpu_affinity else None,
              "p95_published": False, "enabled": {}, "feature_off": {}}
    if args.resume and (output / "summary.json").exists():
        previous = json.loads((output / "summary.json").read_text())
        for key in ("binary_sha256", "threads", "warmups", "measured_runs", "cpu_affinity"):
            if previous.get(key) != report[key]:
                parser.error(f"Cannot resume with changed {key}")
        report = previous
    if args.baseline_exe and (not args.only or args.only == "P0"):
        baseline = args.baseline_exe.resolve()
        baseline_hash = digest(baseline.read_bytes())
        if report.get("baseline_binary_sha256", baseline_hash) != baseline_hash:
            parser.error("Cannot resume with a changed baseline binary")
        report["baseline_binary_sha256"] = baseline_hash
        for case, family in ((case, family) for family in ("bbl", "generic") for case in CASES):
            case_key = f"{case}-{family}"
            if args.resume and case_key in report["feature_off"]:
                continue
            old, new = [], []
            for run in range(args.runs + 1):
                pair = {}
                for label, binary in (("old", baseline), ("new", exe)) if run % 2 == 0 else (("new", exe), ("old", baseline)):
                    pair[label] = capture(binary, output / "P0" / case_key / str(run) / label, case, family)
                if pair["old"]["normalized_gcode_sha256"] != pair["new"]["normalized_gcode_sha256"]:
                    raise RuntimeError(f"Feature-off commands changed for {case}")
                if pair["old"]["move_vertex_bytes"] != pair["new"]["move_vertex_bytes"]:
                    raise RuntimeError("Ordinary preview moves grew in memory")
                if run:
                    old.append(pair["old"]); new.append(pair["new"])
            for samples in (old, new):
                for sample in samples:
                    sample["slicing_export_ms"] = sum(sample[k] for k in ("slice_ms", "paths_ms", "export_ms"))
            a, b = summary(old), summary(new)
            report["feature_off"][case_key] = {"old": a, "new": b,
                "time_ratio": b["slicing_export_ms"]["median"] / a["slicing_export_ms"]["median"],
                "paired_time_ratio": statistics.median(n["slicing_export_ms"] / o["slicing_export_ms"] for o, n in zip(old, new)),
                "memory_ratio": b["peak_working_set_bytes"]["median"] / a["peak_working_set_bytes"]["median"],
                "samples_old": old, "samples_new": new}
            (output / "summary.json").write_text(json.dumps(report, indent=2)+"\n")
            print(f"P0 {case_key}: {report['feature_off'][case_key]['paired_time_ratio']:.3f} paired time, "
                  f"{report['feature_off'][case_key]['time_ratio']:.3f} separate medians, "
                  f"{report['feature_off'][case_key]['memory_ratio']:.3f} RSS", flush=True)
    for spec in suite():
        if args.only and not spec["id"].startswith(args.only):
            continue
        if args.resume and spec["id"] in report["enabled"]:
            if report["enabled"][spec["id"]]["spec"] != spec:
                parser.error(f"Cannot resume changed benchmark {spec['id']}")
            continue
        samples = []
        for run in range(args.runs + 1):
            metrics = benchmark(exe, output / spec["id"] / str(run), spec)
            if run:
                samples.append(metrics)
        report["enabled"][spec["id"]] = {"spec": spec, "summary": summary(samples), "samples": samples}
        (output / "summary.json").write_text(json.dumps(report, indent=2)+"\n")
        metric = "wall_ms" if "wall_ms" in report["enabled"][spec["id"]]["summary"] else "paths_ms"
        print(f"{spec['id']} {metric}: {report['enabled'][spec['id']]['summary'][metric]}", flush=True)
    report["budgets"] = assess(report)
    (output / "summary.json").write_text(json.dumps(report, indent=2)+"\n")
    if report["budgets"]["enabled_failures"]:
        raise RuntimeError("; ".join(report["budgets"]["enabled_failures"]))
    if report["feature_off"] and not report["budgets"]["feature_off_pass"]:
        raise RuntimeError("Feature-off paired time or median peak memory exceeds 5 percent; see summary.json")

if __name__ == "__main__":
    main()
