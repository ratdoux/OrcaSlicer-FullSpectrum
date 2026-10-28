"""Run the opt-in C++ geometry audit on local 3MF projects without rewriting them."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


DEFAULT_MODELS = [
    "Meshy_AI_tyranid_alien_predato_1016020716_image-to-3d-texture.3mf",
    "1_Meshy_AI_tyranid_alien_predato_1016020716_image-to-3d-texture.3mf",
    "3DBenchy.3mf",
    "TestCylinder.3mf",
    "CubeTest.3mf",
    "SlicerProblem_Painting.3mf",
    "Multi-color Cute Dragon by IK3Digital.3mf",
    "Chibi Panther Chameleon by Raki-Box.3mf",
    "1MPolygonBridge.3mf",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--downloads", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", action="append")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--plan-only", action="store_true")
    parser.add_argument("--require-kernel", action="store_true", help="Fail the run if the project falls back to legacy slicing")
    parser.add_argument("--engine", choices=["material_bands", "legacy"], default="material_bands")
    parser.add_argument("--export", action="store_true", help="Export and parse final G-code as well as auditing layer paths")
    args = parser.parse_args()
    binary = args.binary.resolve()
    binary_sha256 = hashlib.sha256(binary.read_bytes()).hexdigest()
    args.output.mkdir(parents=True, exist_ok=True)
    rows = []
    for index, name in enumerate(args.model or DEFAULT_MODELS):
        source = (args.downloads / name).resolve()
        directory = (args.output / f"{index + 1:02d}-{source.stem[:70]}").resolve()
        directory.mkdir(parents=True, exist_ok=True)
        row = {"source": str(source), "output": str(directory), "binary_sha256": binary_sha256}
        if not source.is_file():
            row["status"] = "missing"
        else:
            row["sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
            env = os.environ.copy()
            env.update(MATERIAL_BAND_REAL_MODEL=str(source), MATERIAL_BAND_REAL_MODEL_OUTPUT=str(directory))
            env["MATERIAL_BAND_GEOMETRY_DUMP"] = str(directory)
            env["MATERIAL_BAND_REAL_MODEL_ENGINE"] = args.engine
            env.pop("MATERIAL_BAND_REAL_MODEL_PLAN_ONLY", None)
            env.pop("MATERIAL_BAND_REAL_MODEL_EXPORT", None)
            if args.plan_only:
                env["MATERIAL_BAND_REAL_MODEL_PLAN_ONLY"] = "1"
            if args.export:
                env["MATERIAL_BAND_REAL_MODEL_EXPORT"] = "1"
            print(f"Auditing {source.name}", flush=True)
            start = time.monotonic()
            with (directory / "test.log").open("wb") as log:
                try:
                    result = subprocess.run([str(binary), "[.material_band_real_models]"],
                                            env=env, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                    row.update(status="completed" if result.returncode == 0 else "failed", exit_code=result.returncode)
                except subprocess.TimeoutExpired:
                    row["status"] = "timeout"
            row["elapsed_seconds"] = time.monotonic() - start
            report = directory / "report.json"
            if report.exists():
                row["report"] = json.loads(report.read_text(encoding="utf-8"))
            if args.require_kernel and row["status"] == "completed" and not row.get("report", {}).get("material_band_accepted", False):
                row["status"] = "kernel_rejected"
            row["source_unchanged"] = hashlib.sha256(source.read_bytes()).hexdigest() == row["sha256"]
            if not row["source_unchanged"]:
                row["status"] = "source_changed"
        rows.append(row)
        (args.output / "summary.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
        print(f"{row['status']}: {row.get('report', {}).get('fallback', '')}", flush=True)
    print(f"Evidence: {args.output / 'summary.json'}", flush=True)
    if any(row["status"] != "completed" for row in rows):
        sys.exit(1)


if __name__ == "__main__":
    main()
