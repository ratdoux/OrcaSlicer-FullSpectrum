# FullSpectrum SpectralDB spectrum-estimation experiment

## Outcome

Do not replace the ICC polynomial estimator with a SpectralDB-derived runtime
model yet. The data-driven model is substantially better on held-out opaque
materials and fixes the observed red-family round-trip, but it is spectrally
worse on the calibrated filament anchors. The printed swatches are themselves
effectively opaque because the swatch dimension along the optical path exceeds
the largest TD of the four filaments. The result therefore does not demonstrate an opacity or
translucency mismatch. At most, it suggests a material-domain or measurement-
distribution mismatch between SpectralDB's material population and these
pigmented, printed-polymer swatches; sixteen anchors are not enough to identify
the cause conclusively.

Measured material spectra remain authoritative. ICC remains the fallback for an
unknown RGB color.

## Reproduction

Install NumPy in an isolated Python environment, then run:

```powershell
python scripts/evaluate_spectraldb_estimation.py
```

The script downloads and verifies these immutable inputs:

- C38C/SpectralDB commit
  `7ef373eddcfdd093066293de0c54771d43d6d799`, CSV SHA-256
  `7f639de542c29546a43b61b1aa367e5381364fdd26d793ac405d3a458dfc9232`.
- ICC `xyz2PolyEstimateRefV2.icc`, SHA-256
  `8291983ea02ca7b7adf023a1f3ddd3fc618a853ef20ffd01eb145504a92ff2e4`.

The default JSON and Markdown reports are written to the operating-system temp
directory. Use `--output` and `--markdown` to retain them elsewhere.

## Method

- Parse SpectralDB SCE measurements and resample them to 400-700 nm at 10 nm.
- Reject non-finite spectra and reflectance outside `[0, 1]` (1,284 accepted;
  four rejected).
- Recompute quantized sRGB from each spectrum with the runtime D65/10-degree
  observer path.
- Keep identical quantized colors in the same deterministic five-fold split.
- Separately leave each large measurement source out of training.
- Compare the legacy Gaussian heuristic, current ICC polynomial estimator,
  eight-neighbor SpectralDB lookup, and an eight-component PCA/ridge model.
- Clamp raw estimates to `[0.001, 0.999]` for the metrics, matching the physical
  boundary applied before the existing K/S conversion.

## Results

### Color-grouped five-fold validation

| Model | Mean spectral RMSE | P95 spectral RMSE | Mean CIEDE2000 | P95 CIEDE2000 |
|---|---:|---:|---:|---:|
| Legacy heuristic | 0.06353 | 0.18456 | 6.469 | 19.203 |
| ICC polynomial | 0.03155 | 0.09211 | 1.788 | 4.083 |
| SpectralDB kNN | 0.02409 | 0.07429 | 1.609 | 4.998 |
| SpectralDB PCA/ridge | **0.02246** | **0.05865** | **0.454** | **1.372** |

### Leave-one-measurement-source-out validation

| Model | Mean spectral RMSE | P95 spectral RMSE | Mean CIEDE2000 | P95 CIEDE2000 |
|---|---:|---:|---:|---:|
| Legacy heuristic | 0.06385 | 0.18294 | 6.475 | 19.046 |
| ICC polynomial | 0.03161 | 0.09225 | 1.781 | 4.037 |
| SpectralDB kNN | 0.02829 | 0.08658 | 2.157 | 7.463 |
| SpectralDB PCA/ridge | **0.02376** | **0.05826** | **0.467** | **1.333** |

### Sixteen calibrated filament anchors

| Model | Mean spectral RMSE | Mean CIEDE2000 |
|---|---:|---:|
| Legacy heuristic | 0.13082 | 15.050 |
| ICC polynomial | **0.05695** | 4.000 |
| SpectralDB kNN | 0.07258 | 4.180 |
| SpectralDB PCA/ridge | 0.07625 | **3.622** |

The PCA/ridge model reproduces the apparent color well but misses more of the
actual filament spectrum. Since K/S mixing operates on that spectrum, apparent
single-color agreement alone is not enough to justify replacing ICC. This gap
cannot be attributed to insufficient swatch opacity: the swatch dimension along
the optical path is greater than the largest filament TD. Plausible remaining factors include the
different material populations, pigment and polymer scattering behavior,
surface and print structure, measurement geometry or preprocessing, and the
small sixteen-spectrum anchor set. This benchmark does not distinguish among
those possibilities.

### Problematic color probes

| Input | Legacy | ICC | SpectralDB kNN | SpectralDB PCA/ridge |
|---|---|---|---|---|
| `#C91418` | `#D18D1A` | `#BC1C1A` | `#B61C01` | `#C81815` |
| `#F5ADAD` | `#F8CEA7` | `#EFACAD` | `#D8A8A1` | `#F6ADAD` |
| `#C91818` | `#D18D1A` | `#BC1E1A` | `#B61C00` | `#C81A15` |

This confirms that the old heuristic caused the red-to-orange/green-family
failure. The ICC estimator already removes the catastrophic hue error; the
SpectralDB model improves the single-color round-trip further, but does not yet
provide better filament spectra.

## Data handling and citation

The audited SpectralDB commit contains the CSV and README but no explicit
license file. Therefore the CSV is not vendored and no learned coefficient table
is checked into the runtime. The reproducibility script downloads only the
pinned source and records its hash.

When using the database, cite:

- J. Alstan Jakubiec (2022), *Data-driven selection of typical opaque material
  reflectances for lighting simulation*, LEUKOS,
  DOI: 10.1080/15502724.2022.2100788.
- J. Alstan Jakubiec (2016), *Building a database of opaque materials for
  lighting simulation*, PLEA 2016.

Sources: [C38C/SpectralDB](https://github.com/C38C/SpectralDB),
[SpectralDB about](https://www.spectraldb.com/about.html), and the
[ICC spectral-estimation method](https://www.color.org/resources/spectral/spectral_estimation.xalter).
