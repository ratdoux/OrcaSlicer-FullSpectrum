#!/usr/bin/env python3
"""Generate triple and quadruple KM/K-S residuals from swatch measurements.

The checked-in material profile owns the optically-thick anchors and pair
residuals.  This generator fits the error that remains on black-backed SCE
triple and quadruple sidewall measurements.  Keeping the hierarchy separate
guarantees that higher-order calibration vanishes on pure and pair boundaries.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from dataclasses import dataclass
from itertools import combinations
from pathlib import Path
from typing import Iterable, Sequence

import numpy as np


SPECTRUM_SIZE = 31
FLOAT_PATTERN = re.compile(r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?")


@dataclass(frozen=True)
class BaseProfile:
    colors: tuple[str, ...]
    td_mm: np.ndarray
    material_ks: np.ndarray
    pair_coefficients: dict[tuple[int, int], np.ndarray]


@dataclass(frozen=True)
class Measurement:
    family: tuple[int, ...]
    materials: tuple[int, ...]
    weights: np.ndarray
    measured_ks: np.ndarray
    source: Path
    swatch_id: str


@dataclass(frozen=True)
class HigherOrderFit:
    triple_coefficients: dict[tuple[int, int, int], np.ndarray]
    quadruple_coefficients: dict[tuple[int, int, int, int], np.ndarray]
    triple_condition_numbers: dict[tuple[int, int, int], float]
    quadruple_condition_numbers: dict[tuple[int, int, int, int], float]


def section(text: str, start_marker: str, end_marker: str) -> str:
    start = text.index(start_marker) + len(start_marker)
    end = text.index(end_marker, start)
    return text[start:end]


def parse_base_profile(path: Path) -> BaseProfile:
    text = path.read_text(encoding="utf-8")

    colors_text = section(text, "MATERIAL_HEX = {{", "}};")
    colors = tuple(value.upper() for value in re.findall(r'"(#[0-9A-Fa-f]{6})"', colors_text))

    td_text = section(text, "MATERIAL_TD_MM = {{", "}};")
    td_values = np.asarray([float(value) for value in FLOAT_PATTERN.findall(td_text)], dtype=float)

    ks_text = section(text, "MATERIAL_KS = {{", "struct PairResidualCoefficients")
    ks_values = np.asarray([float(value) for value in FLOAT_PATTERN.findall(ks_text)], dtype=float)
    expected_ks_values = len(colors) * SPECTRUM_SIZE
    if ks_values.size != expected_ks_values:
        raise ValueError(f"expected {expected_ks_values} material K/S values, found {ks_values.size}")
    material_ks = ks_values.reshape((len(colors), SPECTRUM_SIZE))

    pair_text = section(text, "PAIR_RESIDUALS = {{", "}};")
    pair_values = FLOAT_PATTERN.findall(pair_text)
    pair_count_match = re.search(r"PAIR_COUNT\s*=\s*(\d+)", text)
    if pair_count_match is None:
        raise ValueError("PAIR_COUNT is missing from the material profile")
    pair_count = int(pair_count_match.group(1))
    values_per_pair = 2 + 3 * SPECTRUM_SIZE
    if len(pair_values) != pair_count * values_per_pair:
        raise ValueError(
            f"expected {pair_count * values_per_pair} pair values, found {len(pair_values)}"
        )

    pair_coefficients: dict[tuple[int, int], np.ndarray] = {}
    cursor = 0
    for _ in range(pair_count):
        material_a = int(pair_values[cursor])
        material_b = int(pair_values[cursor + 1])
        cursor += 2
        coefficients = np.asarray(
            [float(value) for value in pair_values[cursor : cursor + 3 * SPECTRUM_SIZE]], dtype=float
        ).reshape((3, SPECTRUM_SIZE))
        cursor += 3 * SPECTRUM_SIZE
        pair_coefficients[(material_a, material_b)] = coefficients

    if len(colors) != td_values.size:
        raise ValueError(f"material color/TD count mismatch: {len(colors)} vs {td_values.size}")
    return BaseProfile(colors, td_values, material_ks, pair_coefficients)


def average_sce_spectrum(record: dict) -> np.ndarray | None:
    spectra: list[np.ndarray] = []
    for reading in record.get("measured", {}).get("readings", []):
        values = reading.get("raw_spectrum", {}).get("sce", {}).get("reflectance", [])
        if len(values) != SPECTRUM_SIZE or not any(float(value) > 0.0 for value in values):
            continue
        spectrum = np.asarray(values, dtype=float)
        if np.all(np.isfinite(spectrum)):
            spectra.append(spectrum)
    if not spectra:
        return None
    return np.mean(np.stack(spectra), axis=0)


def material_index(profile: BaseProfile, color: str, td_mm: float) -> int:
    normalized = color.upper()
    matches = [
        index
        for index, (candidate, candidate_td) in enumerate(zip(profile.colors, profile.td_mm))
        if candidate == normalized and math.isclose(float(candidate_td), float(td_mm), abs_tol=0.051)
    ]
    if len(matches) != 1:
        raise ValueError(f"cannot map material {normalized} at TD {td_mm}: candidates={matches}")
    return matches[0]


def load_measurements(directory: Path, profile: BaseProfile) -> tuple[list[Measurement], list[Path]]:
    measurements: dict[tuple[tuple[int, ...], str], Measurement] = {}
    contributing_files: set[Path] = set()

    for path in sorted(directory.rglob("*.json")):
        document = json.loads(path.read_text(encoding="utf-8"))
        # Some exports split one physical swatch into an SCI-only file and an
        # SCE-only file.  Ignore the SCI half before attempting material-ID
        # mapping; combining the two geometries would corrupt the calibration.
        if not any(average_sce_spectrum(record) is not None for record in document.get("records", [])):
            continue
        primary_colors = {int(slot): str(color) for slot, color in document["primary_colors"].items()}
        primary_td = {int(slot): float(td) for slot, td in document["primary_td_values"].items()}
        slot_to_material = {
            slot: material_index(profile, color, primary_td[slot]) for slot, color in primary_colors.items()
        }
        family = tuple(sorted(slot_to_material.values()))

        file_contributed = False
        for record in document.get("records", []):
            manifest = record.get("manifest", {})
            swatch_type = manifest.get("swatch_type")
            if swatch_type not in {"pair_mix", "ternary_mix", "four_color_mix"}:
                continue
            if manifest.get("measurement_condition") != "black_backing":
                continue

            spectrum = average_sce_spectrum(record)
            if spectrum is None:
                continue

            slots = tuple(int(slot) for slot in manifest.get("filament_slots", []))
            ratios = np.asarray(manifest.get("ratios", []), dtype=float)
            if len(slots) not in {2, 3, 4} or ratios.size != len(slots) or np.any(ratios <= 0.0):
                continue

            indices = tuple(slot_to_material[slot] for slot in slots)
            order = np.argsort(indices)
            materials = tuple(indices[int(index)] for index in order)
            weights = ratios[order]
            weights /= np.sum(weights)
            measured_ks = reflectance_to_ks(spectrum)
            swatch_id = str(record.get("swatch_id") or manifest.get("swatch_id"))
            key = (family, swatch_id)
            candidate = Measurement(family, materials, weights, measured_ks, path, swatch_id)

            previous = measurements.get(key)
            if previous is not None:
                if previous.materials != candidate.materials or not np.allclose(previous.weights, candidate.weights):
                    raise ValueError(f"conflicting duplicate swatch metadata for {swatch_id}")
                if not np.allclose(previous.measured_ks, candidate.measured_ks, atol=1e-4, rtol=1e-3):
                    raise ValueError(f"conflicting duplicate SCE spectra for {swatch_id}")
                continue

            measurements[key] = candidate
            file_contributed = True

        if file_contributed:
            contributing_files.add(path)

    result = sorted(measurements.values(), key=lambda item: (item.family, len(item.materials), item.materials, item.swatch_id))
    return result, sorted(contributing_files)


def reflectance_to_ks(reflectance: np.ndarray) -> np.ndarray:
    bounded = np.clip(np.asarray(reflectance, dtype=float), 0.001, 0.999)
    return np.square(1.0 - bounded) / (2.0 * bounded)


def ks_to_reflectance(ks: np.ndarray) -> np.ndarray:
    bounded = np.maximum(np.asarray(ks, dtype=float), 0.0)
    return np.clip(1.0 + bounded - np.sqrt(np.square(bounded) + 2.0 * bounded), 0.0, 1.0)


def pair_correction(profile: BaseProfile, composition: np.ndarray) -> np.ndarray:
    correction = np.zeros(SPECTRUM_SIZE, dtype=float)
    for (material_a, material_b), coefficients in profile.pair_coefficients.items():
        pa = float(composition[material_a])
        pb = float(composition[material_b])
        if pa <= 0.0 or pb <= 0.0:
            continue
        difference = (pa - pb) / (pa + pb)
        basis = np.asarray([1.0, difference, difference * difference])
        correction += pa * pb * (basis @ coefficients)
    return correction


def triple_basis(weights: Sequence[float]) -> np.ndarray:
    return np.asarray(weights, dtype=float)


def quadruple_basis(weights: Sequence[float]) -> np.ndarray:
    return np.asarray(weights, dtype=float)


def composition_for(profile: BaseProfile, measurement: Measurement) -> np.ndarray:
    composition = np.zeros(len(profile.colors), dtype=float)
    for material, weight in zip(measurement.materials, measurement.weights):
        composition[material] = weight
    return composition


def base_pair_prediction(profile: BaseProfile, measurement: Measurement) -> np.ndarray:
    composition = composition_for(profile, measurement)
    return composition @ profile.material_ks + pair_correction(profile, composition)


def ridge_fit(matrix: np.ndarray, targets: np.ndarray, relative_regularization: float) -> tuple[np.ndarray, float]:
    singular_values = np.linalg.svd(matrix, compute_uv=False)
    condition = float(singular_values[0] / singular_values[-1]) if singular_values[-1] > 0.0 else math.inf
    gram = matrix.T @ matrix
    scale = float(np.trace(gram) / max(1, gram.shape[0]))
    regularization = relative_regularization * max(scale, np.finfo(float).eps)
    coefficients = np.linalg.solve(gram + regularization * np.eye(gram.shape[0]), matrix.T @ targets)
    return coefficients, condition


def triple_correction(
    composition: np.ndarray, coefficients: dict[tuple[int, int, int], np.ndarray]
) -> np.ndarray:
    correction = np.zeros(SPECTRUM_SIZE, dtype=float)
    for materials, values in coefficients.items():
        weights = composition[list(materials)]
        if np.any(weights <= 0.0):
            continue
        correction += float(np.prod(weights)) * (triple_basis(weights) @ values)
    return correction


def fit_higher_order(
    profile: BaseProfile, measurements: Sequence[Measurement], relative_regularization: float
) -> HigherOrderFit:
    triple_coefficients: dict[tuple[int, int, int], np.ndarray] = {}
    triple_condition_numbers: dict[tuple[int, int, int], float] = {}

    triple_groups: dict[tuple[int, int, int], list[Measurement]] = {}
    for measurement in measurements:
        if len(measurement.materials) == 3:
            triple_groups.setdefault(measurement.materials, []).append(measurement)

    for materials, samples in sorted(triple_groups.items()):
        matrix = np.stack([float(np.prod(sample.weights)) * triple_basis(sample.weights) for sample in samples])
        targets = np.stack([sample.measured_ks - base_pair_prediction(profile, sample) for sample in samples])
        coefficients, condition = ridge_fit(matrix, targets, relative_regularization)
        triple_coefficients[materials] = coefficients
        triple_condition_numbers[materials] = condition

    quadruple_coefficients: dict[tuple[int, int, int, int], np.ndarray] = {}
    quadruple_condition_numbers: dict[tuple[int, int, int, int], float] = {}
    quadruple_groups: dict[tuple[int, int, int, int], list[Measurement]] = {}
    for measurement in measurements:
        if len(measurement.materials) == 4:
            quadruple_groups.setdefault(measurement.materials, []).append(measurement)

    for materials, samples in sorted(quadruple_groups.items()):
        matrix = np.stack([float(np.prod(sample.weights)) * quadruple_basis(sample.weights) for sample in samples])
        targets = []
        for sample in samples:
            composition = composition_for(profile, sample)
            prediction = base_pair_prediction(profile, sample) + triple_correction(composition, triple_coefficients)
            targets.append(sample.measured_ks - prediction)
        coefficients, condition = ridge_fit(matrix, np.stack(targets), relative_regularization)
        quadruple_coefficients[materials] = coefficients
        quadruple_condition_numbers[materials] = condition

    return HigherOrderFit(
        triple_coefficients,
        quadruple_coefficients,
        triple_condition_numbers,
        quadruple_condition_numbers,
    )


def quadruple_correction(
    composition: np.ndarray, coefficients: dict[tuple[int, int, int, int], np.ndarray]
) -> np.ndarray:
    correction = np.zeros(SPECTRUM_SIZE, dtype=float)
    for materials, values in coefficients.items():
        weights = composition[list(materials)]
        if np.any(weights <= 0.0):
            continue
        correction += float(np.prod(weights)) * (quadruple_basis(weights) @ values)
    return correction


def predict(profile: BaseProfile, fit: HigherOrderFit, measurement: Measurement, order: int) -> np.ndarray:
    prediction = base_pair_prediction(profile, measurement)
    composition = composition_for(profile, measurement)
    if order >= 3:
        prediction += triple_correction(composition, fit.triple_coefficients)
    if order >= 4:
        prediction += quadruple_correction(composition, fit.quadruple_coefficients)
    return prediction


def print_metrics(profile: BaseProfile, fit: HigherOrderFit, measurements: Sequence[Measurement]) -> None:
    print(f"SCE black-backed mixture samples: {len(measurements)}")
    for component_count in (2, 3, 4):
        samples = [sample for sample in measurements if len(sample.materials) == component_count]
        print(f"  {component_count}-component samples: {len(samples)}")
        for order, label in ((2, "pair-only"), (3, "+triple"), (4, "+quadruple")):
            if order > component_count:
                continue
            errors = []
            for sample in samples:
                predicted_reflectance = ks_to_reflectance(predict(profile, fit, sample, order))
                measured_reflectance = ks_to_reflectance(sample.measured_ks)
                errors.append(float(np.sqrt(np.mean(np.square(predicted_reflectance - measured_reflectance)))))
            print(f"    {label:12s} reflectance RMSE: mean={np.mean(errors):.6f}, max={np.max(errors):.6f}")

    for materials, condition in sorted(fit.triple_condition_numbers.items()):
        print(f"  triple {materials} design condition: {condition:.3f}")
    for materials, condition in sorted(fit.quadruple_condition_numbers.items()):
        print(f"  quadruple {materials} design condition: {condition:.3f}")


def format_spectrum(values: Iterable[float], indent: str) -> list[str]:
    values = list(values)
    lines = []
    for start in range(0, len(values), 6):
        chunk = ", ".join(f"{value:.12g}" for value in values[start : start + 6])
        suffix = "," if start + 6 < len(values) else ""
        lines.append(f"{indent}{chunk}{suffix}")
    return lines


def append_entry(lines: list[str], materials: tuple[int, ...], coefficients: np.ndarray, indent: str = "    ") -> None:
    material_text = ", ".join(str(material) for material in materials)
    lines.append(f"{indent}{{")
    lines.append(f"{indent}    {{{{{material_text}}}}},")
    lines.append(f"{indent}    {{{{")
    for coefficient_index, spectrum in enumerate(coefficients):
        lines.append(f"{indent}        {{{{")
        lines.extend(format_spectrum(spectrum, indent + "            "))
        suffix = "," if coefficient_index + 1 < len(coefficients) else ""
        lines.append(f"{indent}        }}}}{suffix}")
    lines.append(f"{indent}    }}}}")
    lines.append(f"{indent}}}")


def source_digest(paths: Sequence[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())
    return digest.hexdigest()


def write_header(output: Path, fit: HigherOrderFit, measurements: Sequence[Measurement], sources: Sequence[Path]) -> None:
    triple_sample_count = sum(len(sample.materials) == 3 for sample in measurements)
    quadruple_sample_count = sum(len(sample.materials) == 4 for sample in measurements)
    pair_sample_count = sum(len(sample.materials) == 2 for sample in measurements)
    lines = [
        "// Generated by scripts/generate_full_spectrum_higher_order_profile.py.",
        "// Scope: 0.08 mm sidewall, SCE spectra, black_backing.",
        f"// Source SHA-256: {source_digest(sources)}",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "",
        "namespace Slic3r::FullSpectrumMaterialHigherOrderProfileData {",
        "",
        f"static constexpr std::size_t SPECTRUM_SIZE = {SPECTRUM_SIZE};",
        "static constexpr std::size_t TRIPLE_BASIS_SIZE = 3;",
        "static constexpr std::size_t QUADRUPLE_BASIS_SIZE = 4;",
        f"static constexpr std::size_t TRIPLE_COUNT = {len(fit.triple_coefficients)};",
        f"static constexpr std::size_t QUADRUPLE_COUNT = {len(fit.quadruple_coefficients)};",
        f"static constexpr std::size_t PAIR_SAMPLE_COUNT = {pair_sample_count};",
        f"static constexpr std::size_t TRIPLE_SAMPLE_COUNT = {triple_sample_count};",
        f"static constexpr std::size_t QUADRUPLE_SAMPLE_COUNT = {quadruple_sample_count};",
        "static constexpr std::size_t TOTAL_MIXTURE_SAMPLE_COUNT =",
        "    PAIR_SAMPLE_COUNT + TRIPLE_SAMPLE_COUNT + QUADRUPLE_SAMPLE_COUNT;",
        "",
        "struct TripleResidualCoefficients",
        "{",
        "    std::array<int, 3> materials {};",
        "    std::array<std::array<double, SPECTRUM_SIZE>, TRIPLE_BASIS_SIZE> coefficients {};",
        "};",
        "",
        "struct QuadrupleResidualCoefficients",
        "{",
        "    std::array<int, 4> materials {};",
        "    std::array<std::array<double, SPECTRUM_SIZE>, QUADRUPLE_BASIS_SIZE> coefficients {};",
        "};",
        "",
        "static constexpr std::array<TripleResidualCoefficients, TRIPLE_COUNT> TRIPLE_RESIDUALS = {{",
    ]
    triple_items = list(sorted(fit.triple_coefficients.items()))
    for index, (materials, coefficients) in enumerate(triple_items):
        append_entry(lines, materials, coefficients)
        if index + 1 < len(triple_items):
            lines[-1] += ","
    lines.extend(["}};", "", "static constexpr std::array<QuadrupleResidualCoefficients, QUADRUPLE_COUNT> QUADRUPLE_RESIDUALS = {{"])
    quadruple_items = list(sorted(fit.quadruple_coefficients.items()))
    for index, (materials, coefficients) in enumerate(quadruple_items):
        append_entry(lines, materials, coefficients)
        if index + 1 < len(quadruple_items):
            lines[-1] += ","
    lines.extend(["}};", "", "} // namespace Slic3r::FullSpectrumMaterialHigherOrderProfileData", ""])
    output.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("measurement_directory", type=Path)
    parser.add_argument(
        "--base-profile",
        type=Path,
        default=Path("src/libslic3r/FullSpectrumMaterialDatabaseProfile.h"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("src/libslic3r/FullSpectrumMaterialHigherOrderProfile.h"),
    )
    parser.add_argument(
        "--relative-regularization",
        type=float,
        default=1.0,
        help="ridge strength relative to the mean design-matrix energy (default selected by leave-one-out validation)",
    )
    args = parser.parse_args()

    profile = parse_base_profile(args.base_profile)
    measurements, sources = load_measurements(args.measurement_directory, profile)
    if not measurements:
        raise SystemExit("no compatible black-backed SCE triple or quadruple measurements found")

    fit = fit_higher_order(profile, measurements, args.relative_regularization)
    print_metrics(profile, fit, measurements)
    print("Contributing SCE files:")
    for source in sources:
        print(f"  {source}")
    write_header(args.output, fit, measurements, sources)
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
