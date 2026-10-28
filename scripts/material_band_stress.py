#!/usr/bin/env python3
"""Deterministic constrained pairwise cases and measured coverage (not Cartesian coverage)."""
import argparse
import itertools
import json
from pathlib import Path
import random
import subprocess
import os

AXES = {
    "regions": [0, 1, 2, 4, 16, 64, 256],
    "recipes": [1, 2, 4, 8, 16],
    "physical": [2, 3, 4],
    "topology": ["stripes", "angular", "islands", "nested", "slivers", "multiway"],
    "height_change": ["constant", "gradient", "birth_death", "component_count"],
    "resolution": [0, 1, 2],
    "process": ["classic", "arachne", "narrow_bead", "thickness_bounds"],
    "state": ["translation", "rotation", "copies", "edit_revert", "insertion_order"],
    "threads": [1, 2, 4],
}

def valid(c):
    if c["recipes"] > max(1, c["regions"]):
        return False
    if c["regions"] == 0:
        return c["topology"] == "stripes" and c["height_change"] == "constant"
    if c["topology"] == "multiway" and (c["regions"] < 2 or c["recipes"] < 2):
        return False
    return True

def pairs(c):
    return {(a, str(c[a]), b, str(c[b])) for a, b in itertools.combinations(AXES, 2)}

def valid_pairs():
    constraints = ("regions", "recipes", "topology", "height_change")
    result = set()
    for a, b in itertools.combinations(AXES, 2):
        for x, y in itertools.product(AXES[a], AXES[b]):
            fixed = {a: x, b: y}
            completions = itertools.product(*([fixed[k]] if k in fixed else AXES[k] for k in constraints))
            if any(valid(dict(zip(constraints, values))) for values in completions):
                result.add((a, str(x), b, str(y)))
    return result

def generate():
    rng = random.Random(20261026)
    candidates = []
    # Random candidates plus every two-axis assignment with a valid completion.
    # The finite universe below establishes valid pair coverage independently of
    # whichever 64 rows the greedy selector happens to choose.
    universe = set()
    constraints = ("regions", "recipes", "topology", "height_change")
    for a, b in itertools.combinations(AXES, 2):
        for x, y in itertools.product(AXES[a], AXES[b]):
            fixed = {a: x, b: y}
            completions = itertools.product(*( [fixed[k]] if k in fixed else AXES[k] for k in constraints))
            if not any(valid(dict(zip(constraints, values))) for values in completions):
                continue
            universe.add((a, str(x), b, str(y)))
            for _ in range(500):
                c = {k: rng.choice(v) for k, v in AXES.items()}
                c.update({a: x, b: y})
                if valid(c):
                    candidates.append(c)
                    break
    for _ in range(10000):
        c = {k: rng.choice(v) for k, v in AXES.items()}
        if valid(c):
            candidates.append(c)
    uncovered, cases = set(universe), []
    # Ensure several accepted production plans; a fallback-only implementation
    # must fail this suite regardless of its pair coverage score.
    for threads in AXES["threads"]:
        c = dict(regions=0, recipes=1, physical=2, topology="stripes", height_change="constant",
                 resolution=0, process="classic", state="edit_revert", threads=threads)
        cases.append(c)
        uncovered -= pairs(c)
    scored = [(c, pairs(c)) for c in candidates]
    while len(cases) < 64:
        index = max(range(len(scored)), key=lambda i: len(scored[i][1] & uncovered))
        c, covered = scored.pop(index)
        cases.append(c)
        uncovered -= covered
    for i, c in enumerate(cases):
        c["seed"] = 20261026 + i
        c["id"] = f"ST{i + 1:02d}"
        c["expected"] = ("dense_classic_profile_required" if c["process"] == "arachne" else
                         "multimaterial_painted_shell_requires_domain_adapter" if c["physical"] > 2 and c["regions"] else
                         "paint_not_matching_full_height_stripes" if c["regions"] else "material_bands")
    return {"schema_version": 1, "seed": 20261026, "axes": AXES, "cases": cases,
            "valid_pair_count": len(universe), "covered_pair_count": len(universe) - len(uncovered),
            "uncovered_valid_pairs": sorted(uncovered),
            "note": "Region axis requests geometric paint patches; metrics also independently count connected recipe components after height edits."}

def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--manifest", type=Path, default=Path("tests/data/material_band/stress.json"))
    parser.add_argument("--test-exe", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build-material-band/evidence/stress"))
    args = parser.parse_args()
    manifest = generate()
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    if args.test_exe:
        args.output.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, MATERIAL_BAND_STRESS_DIR=str(args.output.resolve()))
        with (args.output / "test.log").open("wb") as log:
            subprocess.run([str(args.test_exe.resolve()), "[material_band][stress]", "--rng-seed", "20261026"],
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        results = [json.loads((args.output / c["id"] / "metrics.json").read_text()) for c in manifest["cases"]]
        universe = valid_pairs()
        observed = set()
        for row in results:
            measured = dict(row, regions=row["connected_recipe_components"])
            observed |= pairs(measured)
        # Height edits and touching patches can change connected-component count.
        # Publish measured coverage independently from requested generator levels.
        measured_coverage = {"valid_pair_count": len(universe), "covered_pair_count": len(universe & observed),
                             "uncovered_valid_pairs": sorted(universe - observed),
                             "region_count_mismatches": [r["id"] for r in results if r["regions"] != r["connected_recipe_components"]]}
        (args.output / "coverage.json").write_text(json.dumps({**manifest, "measured_connected_region_coverage": measured_coverage,
                                                               "results": results}, indent=2) + "\n")
    print(f"64 cases: {manifest['covered_pair_count']}/{manifest['valid_pair_count']} valid pairs; see uncovered_valid_pairs")

if __name__ == "__main__":
    main()
