"""Independently reconcile a U1 export with its canonical geometry capture.

The capture contains object paths before brim/tower insertion. Compare every
object segment while allowing validated scheduling order and loader-specific
domain IDs, then reconcile object, brim and tower dose against the completed
real-model report. This is software evidence, not printer testing.
"""

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import re


XYZ_QUANTUM = .001
E_QUANTUM = .00001
NUMBER = re.compile(r"([XYZEF])([-+]?(?:\d+(?:\.\d*)?|\.\d+))")


def segment_key(band, start, finish):
    # Match the writer's round-half-away-from-zero at its XYZ quantum. Exact
    # component and per-segment dose checks follow the lookup; a rounded key
    # alone does not qualify a match.
    coordinates = tuple(int(math.copysign(math.floor(abs(v) * 1000. + .5), v)) for v in start + finish)
    return tuple(band[1:]) + coordinates


def captured_segments(path):
    decoder = json.JSONDecoder()
    with path.open(encoding="utf-8") as stream:
        buffer = stream.read(1048576)
        prefix, buffer = buffer.split('"paths":[', 1)
        header = json.loads(prefix.rstrip(",") + "}")
        diameters = header["diameters"]
        while True:
            buffer = buffer.lstrip(" \t\r\n,")
            if buffer.startswith("]"):
                return
            try:
                item, end = decoder.raw_decode(buffer)
            except json.JSONDecodeError:
                chunk = stream.read(1048576)
                if not chunk:
                    raise
                buffer += chunk
                continue
            buffer = buffer[end:]
            assert len(item["vertices"]) == len(item["segments"]) + 1
            for start, finish, segment in zip(item["vertices"], item["vertices"][1:], item["segments"]):
                yield item["band"], start, finish, segment["values"][0], diameters[item["band"][2] - 1]


def reconcile(capture, report_path, gcode):
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["stage"] == "complete" and report["material_band_accepted"]
    assert report["geometry_source"] == "canonical_material_band_paths"
    assert report["gcode"]["material_band_cycles"] == report["cycles"] > 0, "native preview lost logical cycles"
    diameters = [float(v) for v in report["settings"]["filament_diameter"].split(",")]
    planned = defaultdict(list)
    planned_count = 0
    for expected_band, start, finish, volume, diameter in captured_segments(capture):
        planned[segment_key(expected_band, start, finish)].append((expected_band[0], start, finish, volume, diameter))
        planned_count += 1
    domain_mapping, inverse_domain_mapping = {}, {}
    xyz, tool, e, relative, absolute_xyz = [0., 0., 0.], 0, 0., True, True
    role, band, started = "", None, False
    counts, volumes, bounds = defaultdict(int), defaultdict(float), defaultdict(float)
    maximum_xyz_error, maximum_segment_e_error = 0., 0.
    zero_length, zero_extrusion = 0, 0
    digest = hashlib.sha256()
    with gcode.open("rb") as stream:
        for raw in stream:
            digest.update(raw)
            line = raw.decode("utf-8").strip()
            if line == ";MB_U1_START_END":
                started = True
            if line.startswith(";MB_CYCLE "):
                fields = line.split()
                values = dict(zip(fields[::2], fields[1::2]))
                band = [int(values[k]) for k in ("DOMAIN", ";MB_CYCLE", "MATERIAL", "SLOT")]
            if line.startswith(";MB_OPERATION "):
                role = line.split()[-1]
            code = line.split(";", 1)[0].strip()
            if not code:
                continue
            cmd = code.split()[0]
            fields = {a: float(v) for a, v in NUMBER.findall(code)}
            if cmd == "G90":
                absolute_xyz = True
            elif cmd == "G91":
                absolute_xyz = False
            elif cmd in {"M82", "M83"}:
                relative = cmd == "M83"
            elif re.fullmatch(r"T[0-3]", cmd):
                tool, e = int(cmd[1:]), 0.
            elif cmd == "G92" and "E" in fields:
                e = fields["E"]
            elif cmd in {"G0", "G1"}:
                previous = xyz[:]
                for i, axis in enumerate("XYZ"):
                    if axis in fields:
                        xyz[i] = fields[axis] + (0. if absolute_xyz else xyz[i])
                if "E" not in fields:
                    continue
                amount = fields["E"] if relative else fields["E"] - e
                e += amount
                if not started or role not in {"DEPOSITION", "BRIM", "PRIME", "PURGE"}:
                    continue
                assert absolute_xyz and amount > 0., (role, band, code)
                category = "object" if role == "DEPOSITION" else "brim" if role == "BRIM" else "purge"
                key = (category, tool)
                area = math.pi * diameters[tool] ** 2 / 4.
                e_bound = E_QUANTUM * (.5 if relative else 1.)
                counts[key] += 1
                volumes[key] += amount * area
                bounds[key] += e_bound * area
                if role != "DEPOSITION":
                    continue
                assert tool + 1 == band[2], (band, tool)
                key = segment_key(band, previous, xyz)
                candidates = planned.get(key, [])
                match = None
                for i, (domain, start, finish, volume, diameter) in enumerate(candidates):
                    if (domain_mapping.get(band[0], domain) != domain or
                            inverse_domain_mapping.get(domain, band[0]) != band[0] or diameter != diameters[tool]):
                        continue
                    xyz_error = max(abs(a - b) for a, b in zip(previous + xyz, start + finish))
                    e_error = abs(amount - volume / area)
                    if xyz_error <= XYZ_QUANTUM / 2. + 1e-8 and e_error <= e_bound + 1e-8:
                        match = i, domain, xyz_error, e_error
                        break
                assert match is not None, ("unmatched exported segment", band, previous, xyz, amount, len(candidates))
                i, domain, xyz_error, e_error = match
                domain_mapping[band[0]], inverse_domain_mapping[domain] = domain, band[0]
                candidates.pop(i)
                if not candidates:
                    del planned[key]
                maximum_xyz_error = max(maximum_xyz_error, xyz_error)
                maximum_segment_e_error = max(maximum_segment_e_error, e_error)
                zero_length += xyz == previous
                zero_extrusion += amount == 0.
    assert not planned, ("planned object segments missing from export", sum(map(len, planned.values())))
    comparisons = []
    for category in ("object", "brim", "purge"):
        for tool, target in enumerate(report[category + "_volume_mm3"]):
            key = (category, tool)
            error = abs(volumes[key] - target)
            assert error <= bounds[key] + 1e-7, (key, error, bounds[key])
            comparisons.append(dict(category=category, tool=tool, segments=counts[key], planned_mm3=target,
                                    exported_mm3=volumes[key], absolute_error_mm3=error,
                                    accumulated_rounding_bound_mm3=bounds[key]))
    assert zero_length == 0 and zero_extrusion == 0
    return dict(passed=True, physical_qualification=False, capture=str(capture), report=str(report_path),
                gcode=str(gcode), gcode_sha256=digest.hexdigest(),
                planned_object_segments=planned_count, domain_id_mapping=domain_mapping,
                object_segments=sum(n for (kind, _), n in counts.items() if kind == "object"),
                maximum_xyz_component_error_mm=maximum_xyz_error,
                maximum_segment_e_error_mm=maximum_segment_e_error,
                zero_length_object_segments=zero_length, zero_extrusion_object_segments=zero_extrusion,
                dose=comparisons)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--gcode", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = reconcile(args.capture, args.report, args.gcode)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
