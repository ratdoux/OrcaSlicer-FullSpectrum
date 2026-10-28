"""Audit kernel U1 G-code, including firmware docking absent from normal previews.

The firmware model deliberately leaves XY at a dock and perturbs the incoming
tool's logical Z by 0.15 mm. Explicit Z then XY restoration must remove both.
This checks the exported protocol, not physical printer qualification.
"""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path


def audit(path):
    text = path.read_text(encoding="utf-8")
    assert ";MB_MACHINE Snapmaker U1" in text, path
    assert "G10 P" not in text, path
    assert "MB_U1_BIN" not in text and "BIN_PURGE" not in text, (path, "unexpected bin substitution")
    start, body = text.split(";MB_U1_START_END\n", 1)
    body, ending = body.split(";MB_U1_END_BEGIN\n", 1)
    assert "PRINT_END\nTIMELAPSE_STOP" in ending
    xyz = [0., 0., 0.]
    relative_e, absolute_xyz = True, True
    tool, e = int(re.findall(r"^T([0-3])(?:\s.*)?$", start, re.M)[-1]), 0.
    # Startup macros own unknown intermediate positions; the last explicit
    # absolute commands establish the published initial position and modes.
    for line in start.splitlines()[-10:]:
        tokens = dict((a, float(v)) for a, v in re.findall(r"([XYZEF])([-+]?(?:\d+(?:\.\d*)?|\.\d+))", line))
        if line.startswith("G1 "):
            for i, a in enumerate("XYZ"):
                if a in tokens:
                    xyz[i] = tokens[a]
        if line == "M82":
            relative_e = False
    phase, role, dock_from = "", "", None
    docks, deposits, tools, volumes, highest = 0, 0, set(), [0.] * 4, 0.
    tower_volumes, tower_heights = [0.] * 4, set()
    brim_volumes = [0.] * 4
    selected = False
    bins, bin_from, bin_phase, bin_volumes, bin_heights = 0, None, "", [0.] * 4, []
    dock_heights = []
    temperatures = [0.] * 4
    travel_mm, travel_z_mm, deposition_mm = 0., 0., 0.
    planar_travels, lifted_travels, retracts = 0, 0, 0
    for line in body.splitlines():
        if line == ";MB_U1_BIN_BEGIN":
            assert bin_from is None and dock_from is None
            bin_from, bin_phase = xyz[:], "depart"
            bin_volume_before = sum(bin_volumes)
            bin_heights.append(xyz[2])
            bins += 1
        elif line == ";MB_U1_BIN_END":
            assert bin_phase == "returned" and absolute_xyz
            assert sum(bin_volumes) > bin_volume_before, (path, "bin service omitted its purge extrusion")
            assert all(abs(a - b) < 2e-6 for a, b in zip(xyz, bin_from)), (path, xyz, bin_from)
            bin_from, bin_phase = None, ""
        if line == ";MB_U1_DOCK_BEGIN":
            assert dock_from is None
            dock_from, phase = xyz[:], "select"
            selected = docks == 0
            dock_heights.append(xyz[2])
            docks += 1
        elif line == ";MB_U1_DOCK_END":
            assert phase == "returned" and selected, (path, line, phase)
            assert all(abs(a - b) < 2e-6 for a, b in zip(xyz, dock_from)), (xyz, dock_from)
            dock_from, phase = None, ""
        elif line.startswith(";MB_OPERATION "):
            role = line.split()[-1]
        code = line.split(";", 1)[0].strip()
        if not code:
            continue
        fields = dict((a, float(v)) for a, v in re.findall(r"([XYZEFST])([-+]?(?:\d+(?:\.\d*)?|\.\d+))", code))
        cmd = code.split()[0]
        if cmd == "MOVE_TO_DISCARD_FILAMENT_POSITION":
            assert bin_phase == "depart" and bin_from is not None
            xyz[:2] = [-10., 300.]
            bin_phase = "purging"
        elif cmd in {"INNER_CUTOFF_BASE_DISCARD", "INNER_ROUGHLY_CLEAN_NOZZLE_BASE_DISCARD", "INNER_DISCARD_FILAMENT_BASE_DISCARD"}:
            assert bin_from is not None and bin_phase == "purging"
            # These official macros move XY only and leave G91 active.
            xyz[0] += 5.
            absolute_xyz = False
            if cmd == "INNER_DISCARD_FILAMENT_BASE_DISCARD":
                bin_phase = "restore"
        elif re.fullmatch(r"T[0-3]", cmd):
            assert phase == "select", (path, code)
            selected = True
            absolute_xyz = False
            tool = int(cmd[1:])
            xyz = [35. + tool * 67.5, 250., xyz[2] + .15]
            e = 0.
        elif cmd in {"M104", "M109"}:
            temperatures[int(fields.get("T", tool))] = fields["S"]
        elif cmd == "G90":
            absolute_xyz = True
        elif cmd == "G91":
            absolute_xyz = False
        elif cmd in {"M82", "M83"}:
            relative_e = cmd == "M83"
        elif cmd == "G92" and "E" in fields:
            e = fields["E"]
        elif cmd in {"G0", "G1"}:
            assert absolute_xyz
            previous_xyz = xyz[:]
            if bin_from is not None and bin_phase in {"restore", "z_restored", "returned"}:
                assert "E" not in fields
                if bin_phase == "restore":
                    assert "Z" in fields and not (set(fields) & {"X", "Y"})
                    bin_phase = "z_restored"
                elif bin_phase == "z_restored":
                    assert {"X", "Y"} <= fields.keys() and "Z" not in fields
                    bin_phase = "returned"
                else:
                    raise AssertionError((path, "unexpected bin return motion", code))
            if dock_from is not None:
                assert "E" not in fields
                if phase == "select":
                    assert "Z" in fields and not (set(fields) & {"X", "Y"})
                    phase = "z_restored"
                elif phase == "z_restored":
                    assert {"X", "Y"} <= fields.keys() and "Z" not in fields
                    phase = "returned"
                else:
                    raise AssertionError((path, "unexpected dock motion", code))
            for i, a in enumerate("XYZ"):
                if a in fields:
                    xyz[i] = fields[a]
            distance = math.dist(previous_xyz, xyz)
            if role == "TRAVEL" and dock_from is None and bin_from is None and distance > 0.:
                travel_mm += distance
                travel_z_mm += abs(xyz[2] - previous_xyz[2])
                if abs(xyz[2] - previous_xyz[2]) > 1e-6:
                    lifted_travels += 1
                else:
                    planar_travels += 1
            if "E" in fields:
                amount = fields["E"] if relative_e else fields["E"] - e
                e = e + amount
                retracts += role == "RETRACT" and amount < 0.
                if role == "BIN_PURGE":
                    assert bin_phase == "purging" and amount > 0. and temperatures[tool] >= 170.
                    assert not (set(fields) & {"X", "Y", "Z"})
                    bin_volumes[tool] += amount * math.pi * 1.75 ** 2 / 4.
                if role in {"DEPOSITION", "PRIME", "PURGE", "BRIM"}:
                    assert amount > 0. and temperatures[tool] >= 170., (path, code, temperatures)
                    assert .5 - 1e-6 <= xyz[0] <= 270.5 + 1e-6 and 1. - 1e-6 <= xyz[1] <= 271. + 1e-6
                    assert 0. <= xyz[2] <= 270.05
                    highest = max(highest, xyz[2])
                    deposits += 1
                    deposition_mm += distance
                    tools.add(tool)
                    if role == "DEPOSITION":
                        volumes[tool] += amount * math.pi * 1.75 ** 2 / 4.
                    elif role == "BRIM":
                        brim_volumes[tool] += amount * math.pi * 1.75 ** 2 / 4.
                    else:
                        tower_volumes[tool] += amount * math.pi * 1.75 ** 2 / 4.
                        tower_heights.add(xyz[2])
    assert dock_from is None and deposits > 0 and len(tools) >= 2, (path, dock_from, deposits, tools)
    assert bin_from is None
    assert min(dock_heights) >= highest + 2. - 1e-6, (path, min(dock_heights), highest)
    if bins:
        assert min(bin_heights) >= highest + 2. - 1e-6
        assert sum(bin_volumes) > 0.
    return dict(file=path.name, sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                tools=sorted(tools), dock_returns=docks, deposit_segments=deposits,
                minimum_dock_z=min(dock_heights), highest_deposition_z=highest, object_mm3=volumes,
                tower_mm3=tower_volumes, brim_mm3=brim_volumes, tower_layer_count=len(tower_heights), bin_returns=bins, bin_purge_mm3=bin_volumes,
                travel_mm=travel_mm, travel_z_mm=travel_z_mm, planar_travels=planar_travels,
                lifted_travels=lifted_travels, retractions=retracts, deposition_mm=deposition_mm)


def negative_checks(directory, destination):
    """Ensure incomplete docking transactions fail in both extrusion modes."""
    destination.mkdir(parents=True, exist_ok=True)
    results = []
    for mode, name in [("relative", "U1-painted-4-gradient-box"), ("absolute", "U1-painted-4-caps-box")]:
        source_path = directory / (name + ".gcode")
        audit(source_path)
        source = source_path.read_text(encoding="utf-8")
        block = next((b for b in re.finditer(r";MB_U1_DOCK_BEGIN\n.*?;MB_U1_DOCK_END", source, re.S)
                      if re.search(r"^T[0-3]", b.group(), re.M)), None)
        assert block
        for label, pattern in [
            ("missing_z", r"^G1 Z[^\n]*\n"),
            ("missing_xy", r"^G1 X[^\n]*\n"),
            ("missing_tool", r"^T[0-3][^\n]*\n"),
            ("missing_g90", r"^G90\n"),
        ]:
            changed, count = re.subn(pattern, "", block.group(), flags=re.M)
            assert count
            path = destination / (mode + "-" + label + ".gcode")
            path.write_text(source[:block.start()] + changed + source[block.end():], encoding="utf-8")
            try:
                audit(path)
            except AssertionError:
                results.append(dict(mode=mode, mutation=label, rejected=True))
            else:
                raise AssertionError(f"Audit accepted incomplete docking transaction: {path}")
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--negative-checks", action="store_true")
    args = parser.parse_args()
    rows = [audit(path) for path in sorted(args.directory.glob("*.gcode"))]
    assert rows, "No G-code to audit"
    report = dict(physical_qualification=False, results=rows)
    if args.negative_checks:
        report["negative_checks"] = negative_checks(args.directory, args.output.parent / "audit-negative")
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Passed: {len(rows)} U1 G-code files, {sum(r['dock_returns'] for r in rows)} dock returns")
