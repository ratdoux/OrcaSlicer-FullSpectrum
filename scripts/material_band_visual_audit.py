#!/usr/bin/env python3
"""Render retained centerline audits and exported material-band infill comparisons.

Requires matplotlib. These plots show actual audited/exported paths, not an
illustration of the intended result. They do not establish physical bead shape.
"""

import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection


def project_collage(summary, destination):
    rows = json.loads(summary.read_text(encoding="utf-8"))
    fig, axes = plt.subplots((len(rows) + 2) // 3, 3, figsize=(15, 15), squeeze=False)
    labels = ["Tyranid", "Tyranid (second project)", "Painted Benchy", "Painted cylinder", "Painted cube",
              "Painting problem", "Cute Dragon", "Chameleon", "2.18-million-triangle bridge"]
    for i, (ax, row) in enumerate(zip(axes.flat, rows)):
        report = row.get("report", {})
        all_samples = report.get("samples", [])
        first_object = all_samples[0]["object"] if all_samples else None
        samples = [s for s in all_samples if s["object"] == first_object]
        if not samples:
            ax.set_title(f"{Path(row['source']).stem}\nNo slice samples")
            continue
        sample = samples[len(samples) // 2]
        canonical = "cycle" in sample
        filename = (f"kernel-{first_object}-cycle-{sample['cycle']}.svg" if canonical
                    else f"object-{first_object}-layer-{sample['layer']}.svg")
        svg = Path(row["output"]) / filename
        root = ET.parse(svg).getroot()
        groups = {}
        for line in root.findall("{http://www.w3.org/2000/svg}polyline"):
            # The audit SVG flips Y for its screen coordinate system. Restore
            # model coordinates before labeling the scientific plot axes.
            points = [[float(p.split(",")[0]), -float(p.split(",")[1])] for p in line.attrib["points"].split()]
            groups.setdefault(line.attrib["stroke"], []).append(points)
        for color, lines in groups.items():
            ax.add_collection(LineCollection(lines, colors=color, linewidths=.32))
        ax.autoscale()
        ax.set_aspect("equal")
        label = labels[i] if len(rows) == 9 else Path(row["source"]).stem
        engine = "material bands" if report.get("material_band_accepted") else "legacy fallback"
        section = (f"Cycle {sample['cycle'] + 1}, Z {sample['bottom']:.2f}–{sample['top']:.2f} mm" if canonical
                   else f"Layer {sample['layer'] + 1}, Z {sample['z']:.2f} mm")
        ax.set_title(f"{label}\n{section} | {engine}", fontsize=10)
        ax.set_xlabel("X (mm)")
        ax.set_ylabel("Y (mm)")
    for ax in list(axes.flat)[len(rows):]:
        ax.set_visible(False)
    fig.suptitle("Downloaded projects: middle slice centerlines\nBlue: walls · Orange: infill · Gray: source boundaries", fontsize=15)
    fig.tight_layout(rect=(0, 0, 1, .95))
    fig.savefig(destination, dpi=150)
    plt.close(fig)


def slice_sections(summary, destination):
    """Compare saved sections from one project's actual canonical paths."""
    rows = json.loads(summary.read_text(encoding="utf-8"))
    sections = [(row, sample) for row in rows for sample in row.get("report", {}).get("samples", [])]
    if not sections:
        raise ValueError("The audit has no saved sections")
    columns = min(5, len(sections))
    fig, axes = plt.subplots((len(sections) + columns - 1) // columns, columns,
                             figsize=(4 * columns, 6 * ((len(sections) + columns - 1) // columns)), squeeze=False)
    for ax, (row, sample) in zip(axes.flat, sections):
        canonical = "cycle" in sample
        filename = (f"kernel-{sample['object']}-cycle-{sample['cycle']}.svg" if canonical
                    else f"object-{sample['object']}-layer-{sample['layer']}.svg")
        root = ET.parse(Path(row["output"]) / filename).getroot()
        groups = {}
        for line in root.findall("{http://www.w3.org/2000/svg}polyline"):
            points = [[float(p.split(",")[0]), -float(p.split(",")[1])] for p in line.attrib["points"].split()]
            groups.setdefault(line.attrib["stroke"], []).append(points)
        for color, lines in groups.items():
            ax.add_collection(LineCollection(lines, colors=color, linewidths=.4))
        ax.autoscale()
        ax.set_aspect("equal")
        label = (f"Cycle {sample['cycle'] + 1}\nZ {sample['bottom']:.2f}–{sample['top']:.2f} mm" if canonical
                 else f"Layer {sample['layer'] + 1}\nZ {sample['z']:.2f} mm")
        ax.set_title(f"{Path(row['source']).stem}\n{label}", fontsize=10)
        ax.set_xlabel("X (mm)")
        ax.set_ylabel("Y (mm)")
    for ax in list(axes.flat)[len(sections):]:
        ax.set_visible(False)
    fig.suptitle("Audited slice centerlines\nBlue: perimeters · Orange: infill · Green: brim · Gray: source boundaries", fontsize=14)
    fig.tight_layout(rect=(0, 0, 1, .93))
    fig.savefig(destination, dpi=180)
    plt.close(fig)


def exported_segments(source):
    position = [0., 0., 0.]
    cycle, operation, role, material = -1, "", "", 0
    result = {}
    for line in source.read_text(encoding="utf-8").splitlines():
        if line.startswith(";MB_CYCLE "):
            cycle = int(line.split()[1])
        elif line.startswith(";MB_OPERATION "):
            operation = line.split()[1]
        elif line.startswith(";TYPE:"):
            role = line[6:]
        elif re.fullmatch(r"T\d+", line.strip()):
            material = int(line[1:])
        command = line.split(";", 1)[0].strip().split()
        if not command or command[0] not in ("G0", "G1"):
            continue
        fields = {token[0]: float(token[1:]) for token in command[1:]}
        end = [fields.get(axis, position[i]) for i, axis in enumerate("XYZ")]
        if operation == "DEPOSITION" and "E" in fields and end != position:
            result.setdefault(cycle, []).append((position[:2], end[:2], role, material))
        position = end
    return result


def infill_comparison(directory, destination):
    patterns = ("monotonic", "rectilinear", "concentric")
    fig, axes = plt.subplots(2, 3, figsize=(15, 9), squeeze=False)
    for col, pattern in enumerate(patterns):
        cycles = exported_segments(directory / f"MB02-transition-{pattern}.gcode")
        for row, cycle in enumerate((4, 5)):
            ax = axes[row, col]
            segments = cycles[cycle]
            for kind, color in (("Internal solid infill", "#d97816"), ("External perimeter", "#2255a0")):
                selected = [(a, b) for a, b, role, _ in segments if role == kind]
                ax.add_collection(LineCollection(selected, colors=color, linewidths=.8))
            ax.autoscale()
            ax.set_aspect("equal")
            ax.set_title(f"{pattern.title()} · cycle {cycle + 1} · {(cycle + 1) * .24:.2f} mm")
            ax.set_xlabel("X (mm)")
            ax.set_ylabel("Y (mm)")
    fig.suptitle("MB02 transition: paths parsed from final G-code\nBlue: mixed-material perimeter · Orange: pure-material dense infill", fontsize=15)
    fig.tight_layout(rect=(0, 0, 1, .93))
    fig.savefig(destination, dpi=150)
    plt.close(fig)


def stack_comparison(directory, destination):
    """Reconstruct per-cycle object extrusion from the exported E commands."""
    fig, axes = plt.subplots(2, 2, figsize=(12, 9), squeeze=False)
    colors = ("#c93c42", "#3777c4", "#33a26a", "#d6ae30")
    for row, count in enumerate((3, 4)):
        for col, kind in enumerate(("blend", "gradient")):
            volumes, extruders = {}, {}
            material, cycle, operation, relative = 0, -1, "", False
            for line in (directory / f"MB-{count}-{kind}.gcode").read_text(encoding="utf-8").splitlines():
                if line.startswith(";MB_CYCLE "):
                    cycle = int(line.split()[1])
                elif line.startswith(";MB_OPERATION "):
                    operation = line.split()[1]
                words = line.split(";", 1)[0].split()
                if not words:
                    continue
                if re.fullmatch(r"T\d+", words[0]):
                    material = int(words[0][1:])
                elif words[0] in ("M82", "M83"):
                    relative = words[0] == "M83"
                elif words[0] in ("G0", "G1", "G92"):
                    values = {w[0]: float(w[1:]) for w in words[1:]}
                    if "E" not in values:
                        continue
                    previous = extruders.get(material, 0.)
                    if words[0] == "G92":
                        extruders[material] = values["E"]
                        continue
                    delta = values["E"] if relative else values["E"] - previous
                    extruders[material] = previous + delta
                    if operation == "DEPOSITION" and delta > 0. and any(k in values for k in "XYZ"):
                        volumes.setdefault(cycle, [0.] * count)[material] += delta
            ax = axes[row, col]
            cycles = sorted(volumes)
            bottom = [0.] * len(cycles)
            for material in range(count):
                fractions = [volumes[c][material] / sum(volumes[c]) for c in cycles]
                ax.bar([c + 1 for c in cycles], fractions, bottom=bottom, color=colors[material],
                       width=.8, label=f"Material {material + 1}")
                bottom = [a + b for a, b in zip(bottom, fractions)]
            ax.set_ylim(0., 1.)
            ax.set_xticks([c + 1 for c in cycles])
            ax.set_title(f"{count}-material {kind}")
            ax.set_xlabel("Logical macrocycle (bottom to top)")
            ax.set_ylabel("Fraction of deposited object volume")
            ax.legend(loc="upper center", bbox_to_anchor=(.5, -.17), ncols=2, frameon=False)
    fig.suptitle("Material dose reconstructed from final G-code\nEqual 1.75 mm filament diameters; prime/purge excluded", fontsize=14)
    fig.tight_layout(rect=(0, 0, 1, .94))
    fig.savefig(destination, dpi=150)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--real-summary", type=Path)
    parser.add_argument("--sections-summary", type=Path)
    parser.add_argument("--infill-directory", type=Path)
    parser.add_argument("--stack-directory", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.real_summary:
        project_collage(args.real_summary, args.output / "real-model-middle-layers.png")
    if args.sections_summary:
        slice_sections(args.sections_summary, args.output / "slice-sections.png")
    if args.infill_directory:
        infill_comparison(args.infill_directory, args.output / "mb02-infill-comparison.png")
    if args.stack_directory:
        stack_comparison(args.stack_directory, args.output / "material-stack-dose.png")


if __name__ == "__main__":
    main()
