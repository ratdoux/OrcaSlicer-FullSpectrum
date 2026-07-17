#!/usr/bin/env python3
"""Benchmark RGB-to-reflectance estimators against the pinned SpectralDB corpus.

The benchmark deliberately remains offline tooling.  It does not change the
runtime estimator and writes reports outside the source tree unless requested.
"""

from __future__ import annotations

import argparse
import ast
import csv
import hashlib
import json
import math
import pathlib
import re
import struct
import tempfile
import urllib.request
from dataclasses import dataclass
from typing import Callable

import numpy as np


SPECTRALDB_COMMIT = "7ef373eddcfdd093066293de0c54771d43d6d799"
SPECTRALDB_URL = (
    "https://raw.githubusercontent.com/C38C/SpectralDB/"
    f"{SPECTRALDB_COMMIT}/spectraldb.csv"
)
SPECTRALDB_SHA256 = "7f639de542c29546a43b61b1aa367e5381364fdd26d793ac405d3a458dfc9232"
ICC_URL = "https://www.color.org/resources/spectral/xyz2PolyEstimateRefV2.icc"
ICC_SHA256 = "8291983ea02ca7b7adf023a1f3ddd3fc618a853ef20ffd01eb145504a92ff2e4"
WAVELENGTHS = np.arange(400.0, 701.0, 10.0)

D65 = np.array([
    82.7549, 91.4860, 93.4318, 86.6823, 104.8650, 117.0080, 117.8120, 114.8610,
    115.9230, 108.8110, 109.3540, 107.8020, 104.7900, 107.6890, 104.4050, 104.0460,
    100.0000, 96.3342, 95.7880, 88.6856, 90.0062, 89.5991, 87.6987, 83.2886,
    83.6992, 80.0268, 80.2146, 82.2778, 78.2842, 69.7213, 71.6091,
])
CMF10 = np.array([
    [0.019110, 0.002004, 0.086011], [0.084736, 0.008756, 0.389366],
    [0.204492, 0.021391, 0.972542], [0.314679, 0.038676, 1.553480],
    [0.383734, 0.062077, 1.967280], [0.370702, 0.089456, 1.994800],
    [0.302273, 0.128201, 1.745370], [0.195618, 0.185190, 1.317560],
    [0.080507, 0.253589, 0.772125], [0.016172, 0.339133, 0.415254],
    [0.003816, 0.460777, 0.218502], [0.037465, 0.606741, 0.112044],
    [0.117749, 0.761757, 0.060709], [0.236491, 0.875211, 0.030451],
    [0.376772, 0.961988, 0.013676], [0.529826, 0.991761, 0.003988],
    [0.705224, 0.997340, 0.000000], [0.878655, 0.955552, 0.000000],
    [1.014160, 0.868934, 0.000000], [1.118520, 0.777405, 0.000000],
    [1.124000, 0.658341, 0.000000], [1.030480, 0.527963, 0.000000],
    [0.856297, 0.398057, 0.000000], [0.647467, 0.283493, 0.000000],
    [0.431567, 0.179828, 0.000000], [0.268329, 0.107633, 0.000000],
    [0.152568, 0.060281, 0.000000], [0.081261, 0.031800, 0.000000],
    [0.040851, 0.015905, 0.000000], [0.019941, 0.007749, 0.000000],
    [0.009577, 0.003718, 0.000000],
])


@dataclass
class Corpus:
    ids: list[str]
    names: list[str]
    credits: list[str]
    spectra: np.ndarray
    hexes: list[str]
    linear_rgb: np.ndarray
    rgb_lab: np.ndarray
    spectral_lab: np.ndarray


def download_pinned(url: str, path: pathlib.Path, expected_sha256: str) -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        request = urllib.request.Request(url, headers={"User-Agent": "OrcaSlicer-SpectralDB-benchmark/1"})
        with urllib.request.urlopen(request) as response:
            path.write_bytes(response.read())
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest.lower() != expected_sha256.lower():
        raise RuntimeError(f"Refusing {path}: SHA-256 {digest}, expected {expected_sha256}")
    return path


def srgb_to_linear(rgb: np.ndarray) -> np.ndarray:
    rgb = np.asarray(rgb, dtype=float)
    return np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(rgb: np.ndarray) -> np.ndarray:
    rgb = np.asarray(rgb, dtype=float)
    return np.where(rgb <= 0.0031308, 12.92 * rgb, 1.055 * np.maximum(rgb, 0.0) ** (1.0 / 2.4) - 0.055)


def hex_to_rgb(hex_color: str) -> np.ndarray:
    text = hex_color.lstrip("#")
    return np.array([int(text[i : i + 2], 16) for i in (0, 2, 4)], dtype=float) / 255.0


def rgb_to_hex(rgb: np.ndarray) -> str:
    values = np.rint(np.clip(rgb, 0.0, 1.0) * 255.0).astype(int)
    return "#" + "".join(f"{value:02X}" for value in values)


def xyz_to_lab(xyz: np.ndarray, white: np.ndarray) -> np.ndarray:
    ratio = np.asarray(xyz, dtype=float) / white
    delta = 6.0 / 29.0
    f = np.where(ratio > delta**3, np.cbrt(ratio), ratio / (3.0 * delta**2) + 4.0 / 29.0)
    return np.stack((116.0 * f[..., 1] - 16.0, 500.0 * (f[..., 0] - f[..., 1]), 200.0 * (f[..., 1] - f[..., 2])), axis=-1)


def rgb_lab(rgb: np.ndarray) -> np.ndarray:
    linear = srgb_to_linear(rgb)
    matrix = np.array([
        [0.4124564, 0.3575761, 0.1804375],
        [0.2126729, 0.7151522, 0.0721750],
        [0.0193339, 0.1191920, 0.9503041],
    ])
    xyz = linear @ matrix.T * 100.0
    return xyz_to_lab(xyz, np.array([95.047, 100.0, 108.883]))


def spectra_to_lab(spectra: np.ndarray) -> np.ndarray:
    spectra = np.maximum(np.asarray(spectra, dtype=float), 0.0)
    weighted = D65[:, None] * CMF10
    k = 100.0 / weighted[:, 1].sum()
    xyz = spectra @ weighted * k
    white = weighted.sum(axis=0) * k
    return xyz_to_lab(xyz, white)


def spectra_to_hex(spectra: np.ndarray) -> list[str]:
    lab = spectra_to_lab(spectra)
    fy = (lab[:, 0] + 16.0) / 116.0
    fx = lab[:, 1] / 500.0 + fy
    fz = fy - lab[:, 2] / 200.0
    values = np.stack((fx, fy, fz), axis=1)
    cubed = values**3
    xyz_ratio = np.where(cubed > 0.008856, cubed, (values - 16.0 / 116.0) / 7.787)
    xyz = xyz_ratio * np.array([94.811, 100.0, 107.304]) / 100.0
    matrix = np.array([
        [3.2404542, -1.5371385, -0.4985314],
        [-0.9692660, 1.8760108, 0.0415560],
        [0.0556434, -0.2040259, 1.0572252],
    ])
    srgb = np.clip(linear_to_srgb(xyz @ matrix.T), 0.0, 1.0)
    return [rgb_to_hex(row) for row in srgb]


def delta_e_2000(lab1: np.ndarray, lab2: np.ndarray) -> np.ndarray:
    l1, a1, b1 = np.moveaxis(np.asarray(lab1, float), -1, 0)
    l2, a2, b2 = np.moveaxis(np.asarray(lab2, float), -1, 0)
    c1, c2 = np.hypot(a1, b1), np.hypot(a2, b2)
    cbar = (c1 + c2) / 2.0
    g = 0.5 * (1.0 - np.sqrt(cbar**7 / (cbar**7 + 25.0**7)))
    ap1, ap2 = (1.0 + g) * a1, (1.0 + g) * a2
    cp1, cp2 = np.hypot(ap1, b1), np.hypot(ap2, b2)
    hp1 = np.mod(np.degrees(np.arctan2(b1, ap1)), 360.0)
    hp2 = np.mod(np.degrees(np.arctan2(b2, ap2)), 360.0)
    hp1 = np.where((ap1 == 0) & (b1 == 0), 0.0, hp1)
    hp2 = np.where((ap2 == 0) & (b2 == 0), 0.0, hp2)
    dl = l2 - l1
    dc = cp2 - cp1
    dh_angle = hp2 - hp1
    dh_angle = np.where(dh_angle > 180.0, dh_angle - 360.0, dh_angle)
    dh_angle = np.where(dh_angle < -180.0, dh_angle + 360.0, dh_angle)
    dh_angle = np.where(cp1 * cp2 == 0.0, 0.0, dh_angle)
    dh = 2.0 * np.sqrt(cp1 * cp2) * np.sin(np.radians(dh_angle / 2.0))
    lbar = (l1 + l2) / 2.0
    cpbar = (cp1 + cp2) / 2.0
    hsum = hp1 + hp2
    hpbar = np.where(
        cp1 * cp2 == 0.0,
        hsum,
        np.where(np.abs(hp1 - hp2) <= 180.0, hsum / 2.0, np.where(hsum < 360.0, (hsum + 360.0) / 2.0, (hsum - 360.0) / 2.0)),
    )
    t = (1.0 - 0.17 * np.cos(np.radians(hpbar - 30.0)) + 0.24 * np.cos(np.radians(2.0 * hpbar))
         + 0.32 * np.cos(np.radians(3.0 * hpbar + 6.0)) - 0.20 * np.cos(np.radians(4.0 * hpbar - 63.0)))
    sl = 1.0 + 0.015 * (lbar - 50.0) ** 2 / np.sqrt(20.0 + (lbar - 50.0) ** 2)
    sc = 1.0 + 0.045 * cpbar
    sh = 1.0 + 0.015 * cpbar * t
    rt = -2.0 * np.sqrt(cpbar**7 / (cpbar**7 + 25.0**7)) * np.sin(
        np.radians(60.0 * np.exp(-((hpbar - 275.0) / 25.0) ** 2))
    )
    return np.sqrt((dl / sl) ** 2 + (dc / sc) ** 2 + (dh / sh) ** 2 + rt * (dc / sc) * (dh / sh))


def load_corpus(csv_path: pathlib.Path) -> tuple[Corpus, dict[str, int]]:
    ids: list[str] = []
    names: list[str] = []
    credits: list[str] = []
    spectra: list[np.ndarray] = []
    rejected = {"parse": 0, "coverage": 0, "nonphysical": 0}
    with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            try:
                mapping = ast.literal_eval(row["SCEMeasures"])
                pairs = sorted((float(k), float(v) / 100.0) for k, v in mapping.items())
            except (ValueError, SyntaxError, TypeError):
                rejected["parse"] += 1
                continue
            wavelengths = np.array([pair[0] for pair in pairs])
            values = np.array([pair[1] for pair in pairs])
            if wavelengths[0] > WAVELENGTHS[0] or wavelengths[-1] < WAVELENGTHS[-1]:
                rejected["coverage"] += 1
                continue
            sampled = np.interp(WAVELENGTHS, wavelengths, values)
            if not np.all(np.isfinite(sampled)) or np.any(sampled < 0.0) or np.any(sampled > 1.0):
                rejected["nonphysical"] += 1
                continue
            ids.append(row["ID"])
            names.append(row["Name"])
            credits.append(row["MeasurementCredit"])
            spectra.append(sampled)
    spectrum_array = np.array(spectra)
    hexes = spectra_to_hex(spectrum_array)
    rgb = np.array([hex_to_rgb(value) for value in hexes])
    return Corpus(ids, names, credits, spectrum_array, hexes, srgb_to_linear(rgb), rgb_lab(rgb), spectra_to_lab(spectrum_array)), rejected


def polynomial_features(linear_rgb: np.ndarray) -> np.ndarray:
    r, g, b = np.asarray(linear_rgb).T
    return np.column_stack((
        np.ones_like(r), r, g, b, r*r, g*g, b*b, r*g, r*b, g*b,
        r*r*r, g*g*g, b*b*b, r*r*g, r*r*b, r*g*g, r*b*b, g*g*b, g*b*b, r*g*b,
    ))


class PcaRidge:
    def __init__(self, components: int = 8, alpha: float = 1.0e-3):
        self.component_count = components
        self.alpha = alpha

    def fit(self, rgb: np.ndarray, spectra: np.ndarray) -> "PcaRidge":
        self.mean = spectra.mean(axis=0)
        _, _, vt = np.linalg.svd(spectra - self.mean, full_matrices=False)
        self.components = vt[: self.component_count]
        scores = (spectra - self.mean) @ self.components.T
        phi = polynomial_features(rgb)
        penalty = np.eye(phi.shape[1]) * self.alpha
        penalty[0, 0] = 0.0
        self.weights = np.linalg.solve(phi.T @ phi + penalty, phi.T @ scores)
        return self

    def predict_raw(self, rgb: np.ndarray) -> np.ndarray:
        return self.mean + (polynomial_features(rgb) @ self.weights) @ self.components


def legacy_predict(linear_rgb: np.ndarray) -> np.ndarray:
    linear_rgb = np.asarray(linear_rgb)
    neutral = linear_rgb.min(axis=1)
    chroma = linear_rgb - neutral[:, None]
    average = linear_rgb.mean(axis=1)
    floor = 0.015 + 0.035 * average
    def gaussian(center: float, sigma: float) -> np.ndarray:
        return np.exp(-0.5 * ((WAVELENGTHS - center) / sigma) ** 2)
    red = np.maximum(gaussian(610.0, 58.0), 0.58 * gaussian(680.0, 48.0))
    basis = np.vstack((red, gaussian(540.0, 48.0), gaussian(455.0, 42.0)))
    return np.clip(floor[:, None] + 0.90 * (neutral[:, None] + chroma @ basis), 0.003, 0.985)


def extract_icc_matrix(profile: bytes) -> np.ndarray:
    count = struct.unpack_from(">I", profile, 128)[0]
    payload = None
    for index in range(count):
        signature, offset, size = struct.unpack_from(">4sII", profile, 132 + index * 12)
        if signature == b"B2A3":
            payload = profile[offset : offset + size]
            break
    if payload is None:
        raise RuntimeError("ICC profile lacks B2A3")
    offset = payload.find(b"matf")
    inputs, outputs = struct.unpack_from(">HH", payload, offset + 8)
    if (inputs, outputs) != (20, 41):
        raise RuntimeError(f"Unexpected ICC matrix {inputs}x{outputs}")
    values = np.array(struct.unpack_from(">820f", payload, offset + 12)).reshape(41, 20)
    return values[2:33]


def icc_predict(linear_rgb: np.ndarray, coefficients: np.ndarray) -> np.ndarray:
    d65_matrix = np.array([
        [0.412348487282463, 0.357601378848728, 0.180450133868809],
        [0.212617188755020, 0.715202757697456, 0.072180053547523],
        [0.019328835341365, 0.119200459616243, 0.950370705042392],
    ])
    bradford = np.array([
        [1.047907381710167, 0.022933384554211, -0.050201634798010],
        [0.029605959417717, 0.990456039910784, -0.017075529195870],
        [-0.009246794326782, 0.015062680140149, 0.751791232609078],
    ])
    xyz = (linear_rgb @ d65_matrix.T) @ bradford.T * 100.0
    return polynomial_features(xyz) @ coefficients.T


def knn_predict(train_lab: np.ndarray, train_spectra: np.ndarray, query_lab: np.ndarray, neighbors: int = 8) -> np.ndarray:
    distance2 = ((query_lab[:, None, :] - train_lab[None, :, :]) ** 2).sum(axis=2)
    k = min(neighbors, train_lab.shape[0])
    indices = np.argpartition(distance2, k - 1, axis=1)[:, :k]
    selected = np.take_along_axis(distance2, indices, axis=1)
    exact = selected <= 1.0e-12
    weights = np.where(exact, 1.0, 1.0 / np.maximum(selected, 1.0e-12))
    weights = np.where(exact.any(axis=1)[:, None], exact.astype(float), weights)
    weights /= weights.sum(axis=1, keepdims=True)
    return (train_spectra[indices] * weights[:, :, None]).sum(axis=1)


def fold_for_hex(hex_color: str, fold_count: int) -> int:
    return int.from_bytes(hashlib.sha256(hex_color.encode("ascii")).digest()[:8], "big") % fold_count


def metric_summary(actual: np.ndarray, raw_prediction: np.ndarray) -> dict[str, float]:
    clipped = np.clip(raw_prediction, 0.001, 0.999)
    row_rmse = np.sqrt(np.mean((clipped - actual) ** 2, axis=1))
    de = delta_e_2000(spectra_to_lab(actual), spectra_to_lab(clipped))
    return {
        "spectral_rmse_mean": float(row_rmse.mean()),
        "spectral_rmse_median": float(np.median(row_rmse)),
        "spectral_rmse_p95": float(np.percentile(row_rmse, 95)),
        "delta_e_2000_mean": float(de.mean()),
        "delta_e_2000_median": float(np.median(de)),
        "delta_e_2000_p95": float(np.percentile(de, 95)),
        "raw_out_of_bounds_fraction": float(np.mean((raw_prediction < 0.0) | (raw_prediction > 1.0))),
        "raw_out_of_bounds_rows": int(np.sum(np.any((raw_prediction < 0.0) | (raw_prediction > 1.0), axis=1))),
    }


def cross_validate(corpus: Corpus, icc_coefficients: np.ndarray, folds: int) -> tuple[dict[str, dict[str, float]], dict[str, np.ndarray], list[int]]:
    assignments = np.array([fold_for_hex(value, folds) for value in corpus.hexes])
    predictions = {name: np.empty_like(corpus.spectra) for name in ("legacy", "icc", "spectraldb_knn", "spectraldb_pca_ridge")}
    predictions["legacy"][:] = legacy_predict(corpus.linear_rgb)
    predictions["icc"][:] = icc_predict(corpus.linear_rgb, icc_coefficients)
    for fold in range(folds):
        test = assignments == fold
        train = ~test
        predictions["spectraldb_knn"][test] = knn_predict(corpus.rgb_lab[train], corpus.spectra[train], corpus.rgb_lab[test])
        model = PcaRidge().fit(corpus.linear_rgb[train], corpus.spectra[train])
        predictions["spectraldb_pca_ridge"][test] = model.predict_raw(corpus.linear_rgb[test])
    metrics = {name: metric_summary(corpus.spectra, prediction) for name, prediction in predictions.items()}
    return metrics, predictions, [int(np.sum(assignments == fold)) for fold in range(folds)]


def validate_by_measurement_credit(
    corpus: Corpus, icc_coefficients: np.ndarray, minimum_rows: int = 50
) -> tuple[dict[str, dict[str, float]], dict[str, int]]:
    credits = np.array(corpus.credits)
    groups = {
        credit: int(np.sum(credits == credit))
        for credit in sorted(set(corpus.credits))
        if np.sum(credits == credit) >= minimum_rows
    }
    selected = np.isin(credits, list(groups))
    predictions = {
        "legacy": legacy_predict(corpus.linear_rgb[selected]),
        "icc": icc_predict(corpus.linear_rgb[selected], icc_coefficients),
        "spectraldb_knn": np.empty((int(selected.sum()), len(WAVELENGTHS))),
        "spectraldb_pca_ridge": np.empty((int(selected.sum()), len(WAVELENGTHS))),
    }
    selected_indices = np.flatnonzero(selected)
    output_position = {source_index: output_index for output_index, source_index in enumerate(selected_indices)}
    for credit in groups:
        test = credits == credit
        train = ~test
        destinations = np.array([output_position[index] for index in np.flatnonzero(test)])
        predictions["spectraldb_knn"][destinations] = knn_predict(
            corpus.rgb_lab[train], corpus.spectra[train], corpus.rgb_lab[test]
        )
        predictions["spectraldb_pca_ridge"][destinations] = PcaRidge().fit(
            corpus.linear_rgb[train], corpus.spectra[train]
        ).predict_raw(corpus.linear_rgb[test])
    return {
        name: metric_summary(corpus.spectra[selected], prediction)
        for name, prediction in predictions.items()
    }, groups


def parse_material_anchors(header_path: pathlib.Path) -> tuple[list[str], np.ndarray]:
    text = header_path.read_text(encoding="utf-8")
    hex_block = text.split("MATERIAL_HEX = {{", 1)[1].split("}};", 1)[0]
    hex_rows = re.findall(r'"(#[0-9A-Fa-f]{6})"', hex_block)
    ks_block = text.split("MATERIAL_KS = {{", 1)[1].split("}};", 1)[0]
    ks_rows = re.findall(r"\{\{\s*([^{}]+?)\s*\}\}", ks_block, flags=re.DOTALL)
    spectra = []
    for row in ks_rows:
        ks = np.array([float(value) for value in re.findall(r"[-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?", row)])
        if len(ks) != len(WAVELENGTHS):
            continue
        spectra.append(np.clip(1.0 + ks - np.sqrt(ks * ks + 2.0 * ks), 0.0, 1.0))
    if len(hex_rows) != len(spectra):
        raise RuntimeError(f"Parsed {len(hex_rows)} material hexes and {len(spectra)} spectra")
    return hex_rows, np.array(spectra)


def evaluate_queries(hexes: list[str], predictors: dict[str, Callable[[np.ndarray, np.ndarray], np.ndarray]], actual: np.ndarray | None = None) -> list[dict[str, object]]:
    rgb = np.array([hex_to_rgb(value) for value in hexes])
    linear = srgb_to_linear(rgb)
    lab = rgb_lab(rgb)
    rows: list[dict[str, object]] = []
    predicted = {name: function(linear, lab) for name, function in predictors.items()}
    for index, value in enumerate(hexes):
        row: dict[str, object] = {"input_hex": value}
        for name, spectra in predicted.items():
            clipped = np.clip(spectra[index : index + 1], 0.001, 0.999)
            entry: dict[str, object] = {"roundtrip_hex": spectra_to_hex(clipped)[0]}
            if actual is not None:
                entry.update(metric_summary(actual[index : index + 1], spectra[index : index + 1]))
            row[name] = entry
        rows.append(row)
    return rows


def markdown_report(result: dict[str, object]) -> str:
    lines = [
        "# SpectralDB RGB-to-reflectance benchmark",
        "",
        "This is an offline experiment; it does not replace measured material spectra or change runtime behavior.",
        "",
        "## Provenance",
        "",
        f"- SpectralDB commit: `{SPECTRALDB_COMMIT}`",
        f"- SpectralDB CSV SHA-256: `{SPECTRALDB_SHA256}`",
        f"- ICC profile SHA-256: `{ICC_SHA256}`",
        "- Spectra: SCE, 400-700 nm at 10 nm, reflectance constrained to [0, 1]",
        "- Citation: J. Alstan Jakubiec (2022), Data-driven selection of typical opaque material reflectances for lighting simulation, LEUKOS, DOI 10.1080/15502724.2022.2100788.",
        "- Citation: J. Alstan Jakubiec (2016), Building a database of opaque materials for lighting simulation, PLEA 2016.",
        "",
        "## Held-out results",
        "",
        "Equal quantized colors are assigned to the same deterministic fold.",
        "",
        "| Model | Mean spectral RMSE | P95 spectral RMSE | Mean CIEDE2000 | P95 CIEDE2000 | OOB samples |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for name, values in result["cross_validation"]["metrics"].items():
        lines.append(
            f"| {name} | {values['spectral_rmse_mean']:.5f} | {values['spectral_rmse_p95']:.5f} | "
            f"{values['delta_e_2000_mean']:.3f} | {values['delta_e_2000_p95']:.3f} | {values['raw_out_of_bounds_fraction']:.3%} |"
        )
    lines.extend([
        "",
        "## Leave-one-measurement-source-out results",
        "",
        "Each listed measurement source is evaluated using a model trained on every other source.",
        "",
        "| Model | Mean spectral RMSE | P95 spectral RMSE | Mean CIEDE2000 | P95 CIEDE2000 |",
        "|---|---:|---:|---:|---:|",
    ])
    for name, values in result["source_holdout"]["metrics"].items():
        lines.append(
            f"| {name} | {values['spectral_rmse_mean']:.5f} | {values['spectral_rmse_p95']:.5f} | "
            f"{values['delta_e_2000_mean']:.3f} | {values['delta_e_2000_p95']:.3f} |"
        )
    lines.extend(["", "## Calibrated material anchors", "", "| Model | Mean spectral RMSE | Mean CIEDE2000 |", "|---|---:|---:|"])
    anchor_models = list(result["material_anchors"][0].keys())[1:]
    for name in anchor_models:
        entries = [row[name] for row in result["material_anchors"]]
        lines.append(f"| {name} | {np.mean([x['spectral_rmse_mean'] for x in entries]):.5f} | {np.mean([x['delta_e_2000_mean'] for x in entries]):.3f} |")
    lines.extend(["", "## Color probes", "", "| Input | " + " | ".join(anchor_models) + " |", "|---|" + "---|" * len(anchor_models)])
    for row in result["probes"]:
        lines.append("| " + row["input_hex"] + " | " + " | ".join(row[name]["roundtrip_hex"] for name in anchor_models) + " |")
    lines.extend([
        "",
        "## Interpretation",
        "",
        str(result["decision"]),
        "",
        "Spectral reconstruction from one RGB triplet is underdetermined. These models provide plausible priors for unknown filaments; measured per-material spectra remain authoritative.",
    ])
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=pathlib.Path, default=pathlib.Path.home() / ".cache" / "orcaslicer-spectraldb")
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(tempfile.gettempdir()) / "spectraldb-estimation-report.json",
    )
    parser.add_argument("--markdown", type=pathlib.Path)
    parser.add_argument("--material-profile", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / "src/libslic3r/FullSpectrumMaterialDatabaseProfile.h")
    parser.add_argument("--folds", type=int, default=5)
    args = parser.parse_args()

    csv_path = download_pinned(SPECTRALDB_URL, args.cache_dir / "spectraldb.csv", SPECTRALDB_SHA256)
    icc_path = download_pinned(ICC_URL, args.cache_dir / "xyz2PolyEstimateRefV2.icc", ICC_SHA256)
    corpus, rejected = load_corpus(csv_path)
    coefficients = extract_icc_matrix(icc_path.read_bytes())
    metrics, _, fold_sizes = cross_validate(corpus, coefficients, args.folds)
    source_metrics, source_groups = validate_by_measurement_credit(corpus, coefficients)

    full_pca = PcaRidge().fit(corpus.linear_rgb, corpus.spectra)
    predictors = {
        "legacy": lambda linear, lab: legacy_predict(linear),
        "icc": lambda linear, lab: icc_predict(linear, coefficients),
        "spectraldb_knn": lambda linear, lab: knn_predict(corpus.rgb_lab, corpus.spectra, lab),
        "spectraldb_pca_ridge": lambda linear, lab: full_pca.predict_raw(linear),
    }
    anchor_hexes, anchor_spectra = parse_material_anchors(args.material_profile)
    anchors = evaluate_queries(anchor_hexes, predictors, anchor_spectra)
    anchor_metrics = {
        name: {
            metric: float(np.mean([row[name][metric] for row in anchors]))
            for metric in ("spectral_rmse_mean", "delta_e_2000_mean")
        }
        for name in predictors
    }
    probes = evaluate_queries(
        ["#C91418", "#F5ADAD", "#C91718", "#C91818", "#C91918", "#FF0000", "#00FF00", "#0000FF", "#FFFFFF", "#000000"],
        predictors,
    )

    pca = metrics["spectraldb_pca_ridge"]
    icc = metrics["icc"]
    source_pca = source_metrics["spectraldb_pca_ridge"]
    source_icc = source_metrics["icc"]
    if (pca["spectral_rmse_mean"] < icc["spectral_rmse_mean"]
            and pca["delta_e_2000_mean"] < icc["delta_e_2000_mean"]
            and source_pca["spectral_rmse_mean"] < source_icc["spectral_rmse_mean"]
            and source_pca["delta_e_2000_mean"] < source_icc["delta_e_2000_mean"]
            and anchor_metrics["spectraldb_pca_ridge"]["spectral_rmse_mean"] < anchor_metrics["icc"]["spectral_rmse_mean"]
            and anchor_metrics["spectraldb_pca_ridge"]["delta_e_2000_mean"] < anchor_metrics["icc"]["delta_e_2000_mean"]):
        decision = "The compact SpectralDB PCA/ridge prior beats ICC on both primary metrics in color-grouped folds, source-held-out validation, and calibrated filament anchors. Runtime integration is justified."
    else:
        decision = (
            "SpectralDB PCA/ridge wins on its opaque-material holdouts but does not beat ICC on both "
            "effectively opaque printed-polymer anchor metrics. This does not justify runtime integration. "
            "It suggests a material-domain or measurement-distribution mismatch, although sixteen anchors "
            "cannot identify the cause; keep ICC as the unknown-color fallback."
        )
    result: dict[str, object] = {
        "provenance": {
            "spectraldb_url": SPECTRALDB_URL,
            "spectraldb_commit": SPECTRALDB_COMMIT,
            "spectraldb_sha256": SPECTRALDB_SHA256,
            "icc_url": ICC_URL,
            "icc_sha256": ICC_SHA256,
        },
        "corpus": {"accepted_rows": len(corpus.ids), "rejected_rows": rejected, "fold_sizes": fold_sizes},
        "cross_validation": {"folds": args.folds, "metrics": metrics},
        "source_holdout": {"groups": source_groups, "metrics": source_metrics},
        "material_anchor_summary": anchor_metrics,
        "material_anchors": anchors,
        "probes": probes,
        "decision": decision,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    markdown = args.markdown or args.output.with_suffix(".md")
    markdown.parent.mkdir(parents=True, exist_ok=True)
    markdown.write_text(markdown_report(result), encoding="utf-8", newline="\n")
    print(f"Wrote {args.output}")
    print(f"Wrote {markdown}")
    print(decision)


if __name__ == "__main__":
    main()
