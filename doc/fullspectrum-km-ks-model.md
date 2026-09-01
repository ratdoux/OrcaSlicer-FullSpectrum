# FullSpectrum KM/K-S Color Prediction

## What KM/K-S means

KM is the Kubelka-Munk model. It describes a diffusely reflecting material using:

- \(K(\lambda)\): absorption coefficient
- \(S(\lambda)\): scattering coefficient
- \(R(\lambda)\): measured reflectance at wavelength \(\lambda\)

For an optically thick material, the remission function is:

\[
F(R)=\frac{K}{S}=\frac{(1-R)^2}{2R}
\]

The inverse converts a mixed K/S value back to reflectance:

\[
R=1+F-\sqrt{F^2+2F}
\]

This is implemented directly in [FullSpectrumKSPairResidual.cpp](../src/libslic3r/FullSpectrumKSPairResidual.cpp#L159), with the inverse at [line 232](../src/libslic3r/FullSpectrumKSPairResidual.cpp#L232).

The useful property is that mixtures are more nearly linear in K/S space than in RGB:

\[
F_{\text{mix}}(\lambda)=\sum_i w_iF_i(\lambda)
\]

This explains why a dark or strongly absorbing filament can dominate a mixture. For example, at one wavelength, mixing reflectances 0.2 and 0.8 at 60/40 gives a KM reflectance of about 0.27—not the RGB-style average of 0.44.

Strictly, full two-constant KM mixing would mix \(K\) and \(S\) separately. This implementation stores and blends only \(K/S\), effectively assuming comparable scattering strengths. Calibration residuals compensate for some of that approximation.

## The repository's complete prediction pipeline

```text
hex + percentage + TD + material ID
    → measured or estimated 31-point spectrum
    → reflectance-to-K/S transform
    → TD-adjusted normalized weights
    → linear K/S mixture
    → calibrated pair, triple, and quadruple residuals
    → inverse K/S-to-reflectance
    → D65/CIE XYZ and Lab
    → displayable sRGB hex
```

It operates at 31 wavelengths from 400–700 nm in 10 nm increments.

### 1. Obtain each filament spectrum

For a recognized calibrated filament, the implementation uses measured K/S data from the embedded database. It currently contains 16 materials and 24 calibrated pairs, divided into four four-material families. The measurements are described as 0.08 mm sidewalls, SCE, black-backed in [FullSpectrumMaterialDatabaseProfile.h](../src/libslic3r/FullSpectrumMaterialDatabaseProfile.h#L1).

Material recognition works in this order:

1. A stable FullSpectrum material ID, if supplied, is authoritative.
2. Otherwise, an exact measured display hex may identify the material.
3. If a TD is supplied during color inference, it must be reasonably close to the material's native TD.
4. Generic colors such as `#FFFFFF` or `#0000FF` are never identified by color alone because they are too ambiguous.
5. An unknown or stale explicit ID fails closed—it does not regain calibrated status merely because the hex matches.

This logic starts in [FullSpectrumKSPairResidual.cpp](../src/libslic3r/FullSpectrumKSPairResidual.cpp#L93).

For an uncalibrated but valid `#RRGGBB` color, the slicer estimates a reflectance spectrum using an ICC Munsell polynomial profile:

\[
\text{sRGB}\rightarrow XYZ_{D65}\rightarrow XYZ_{D50}
\rightarrow \text{20-term polynomial}\rightarrow R(400\ldots700)
\]

See [FullSpectrumICCPolynomialEstimator.cpp](../src/libslic3r/FullSpectrumICCPolynomialEstimator.cpp#L53). That estimated reflectance is then converted into K/S normally. Invalid hex input causes the high-level manager to fall back to the older RGB FilamentMixer.

### 2. Apply transmission-distance correction

For each component:

\[
u_i=p_i\frac{TD_{\text{reference},i}}{TD_{\text{runtime},i}},
\qquad
w_i=\frac{u_i}{\sum_j u_j}
\]

Here \(p_i\) is the requested recipe percentage.

- Smaller runtime TD means a stronger, less-transmissive optical contribution.
- Larger TD weakens the contribution.
- A calibrated material uses its native measured TD as the reference.
- An uncalibrated color uses 6 mm.
- At the native TD, the correction is neutral.

This is a practical optical-strength adjustment, not a full finite-thickness Kubelka-Munk transmission calculation. It is implemented in [FullSpectrumKSPairResidual.cpp](../src/libslic3r/FullSpectrumKSPairResidual.cpp#L181).

### 3. Mix K/S and add learned interaction residuals

The base prediction at every wavelength is:

\[
F_{\text{base}}(\lambda)=\sum_i w_iF_i(\lambda)
\]

For calibrated material pairs, the code then adds a learned wavelength-dependent correction:

\[
\Delta F_{ab}(\lambda)
=
p_ap_b
\left[
b_{0,\lambda}
+b_{1,\lambda}d
+b_{2,\lambda}d^2
\right]
\]

where:

\[
d=\frac{p_a-p_b}{p_a+p_b}
\]

The \(p_ap_b\) factor makes the correction vanish at either pure endpoint, so measured single-material anchors remain unchanged. Pair corrections remain the boundary model for mixtures of any size. They are followed by explicitly measured higher-order corrections when the complete material combination has a calibrated entry.

For a calibrated triple, the residual remaining after the base and all three pair terms is fitted as:

\[
\Delta F_{abc}(\lambda)=p_ap_bp_c
\left[c_{a,\lambda}p_a+c_{b,\lambda}p_b+c_{c,\lambda}p_c\right]
\]

For a calibrated quadruple, the residual remaining after the base, six pair terms, and four triple terms is fitted equivalently:

\[
\Delta F_{abcd}(\lambda)=p_ap_bp_cp_d
\sum_{i\in\{a,b,c,d\}}q_{i,\lambda}p_i
\]

The product factors make each correction disappear whenever one of its components reaches zero. As a result, triple calibration cannot alter pure or pair predictions, and quadruple calibration cannot alter any simplex face. The barycentric fields use ridge regularization selected by leave-one-out validation; this avoids the nonphysical interpolation produced by a higher-degree exact fit.

The checked-in higher-order profile was generated from 391 compatible SCE, black-backed mixture measurements: 162 pairs already represented by the pair profile, 120 triples, and 109 quadruples. Three four-material families have higher-order measurements, producing 12 triple combinations and 3 quadruple combinations. The fourth embedded family retains its calibrated pair model until matching higher-order measurements are supplied.

One Panchroma swatch export is split across an SCE file and an SCI file. The generator recognizes the split and selects only the SCE spectra; SCI and SCE are never averaged together. Regenerate the profile with:

```powershell
python scripts/generate_full_spectrum_higher_order_profile.py G:\Measurements
```

Unknown materials and unmeasured cross-family combinations receive only the calibration terms whose complete material subsets are known.

The runtime interaction calculation is in [FullSpectrumKSPairResidual.cpp](../src/libslic3r/FullSpectrumKSPairResidual.cpp), and the generated higher-order coefficients are in [FullSpectrumMaterialHigherOrderProfile.h](../src/libslic3r/FullSpectrumMaterialHigherOrderProfile.h).

### 4. Integrate the spectrum into a visible color

The predicted reflectance spectrum is numerically integrated under D65 illumination using the CIE 1964 10° observer:

\[
X=k\sum_\lambda R(\lambda)E_{D65}(\lambda)\bar{x}(\lambda)
\]

Equivalent equations are used for \(Y\) and \(Z\). The result becomes CIELAB and then sRGB, with out-of-gamut channels clipped to `[0, 1]`. See [FullSpectrumKSPairResidual.cpp](../src/libslic3r/FullSpectrumKSPairResidual.cpp#L279).
