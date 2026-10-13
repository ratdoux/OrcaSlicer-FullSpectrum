# FullSpectrum migration: requirements and development tasks

This retrospective organizes the FullSpectrum 0.9.13 to Snorca port under the four original tracker categories and a fifth category for Xipit's UI rework integration. It preserves all 24 numbered proposal requirements and their 32 development tasks, and adds 10 UI-rework requirements and 10 development tasks from `proposal_UI_changes`, for a total of 34 requirements and 42 tasks.

For every development task below, use **Phase: In development** and **Status: Completed (retrospective)**. The phase identifies the type of work; the status records that development has already been completed. Completion follows the project owner's statement and the implementation records. It does not imply that every release acceptance check or physical print has been signed off.

Source proposal: `C:\Users\radug\Downloads\FullSpectrum-0.9.13-to-Snorca-Product-Port-Proposal.html`. Implementation reviewed: branch `product_proposal_merge_with_mf_refactor`, through commit `2740cbd425`.

UI-rework extension: committed changes introduced in `proposal_UI_changes` by `0ba1d67f11`, compared with `2740cbd425`. The fifth category covers the community UI adaptation and associated Snorca palette and inspection work integrated in that commit. The original gradient editor remains under GR-01–GR-06. The later preview-memory/GPU commit `9a791daff1` and the ongoing CPU/GPU optimization work are outside this UI category.

| Category | Source scope | Requirements | Development tasks |
| --- | --- | ---: | ---: |
| FS migration - gradient color logic optimization | 3.1–3.6 | 6 | 8 |
| FS migration - mixed color preview prediction feature | 1.1–1.4 | 4 | 6 |
| FS migration - UI/UX optimization | 4.1–4.6 | 6 | 8 |
| FS migration - SML algorithm optimization | 2.1–2.8 | 8 | 10 |
| FS migration - Xipit UI rework integration | UI additions in `0ba1d67f11` | 10 | 10 |
| Total | Original proposal plus UI-rework extension | 34 | 42 |

Ownership follows the feature: gradient editing and rendering stay in the gradient category; the common prediction engine belongs to color prediction; general settings and surface-painting tools belong to UI/UX; pass planning and tool changes belong to SML. The Xipit UI category owns the added sidebar recipe presentation, filament-family palette flow, and color-identification interaction. Shared integration work is included within these five categories.

## 1. Itemized requirements

The tables describe the requested product behavior. Implementation decisions that refine the original proposal are recorded immediately afterward.

### FS migration - gradient color logic optimization

Port FullSpectrum's gradient design and preview workflow to support ordered sequences of two, three, or four filaments. Users must be able to position color stops, adjust transition midpoints, and define a centered solid-color region at each internal stop, with widths constrained by neighboring transitions. Provide layered and blended previews in the gradient editor, together with vertical gradient rendering on whole models and painted regions in Prepare. Edits must update the relevant previews and slicing inputs consistently. Correct transitions between successive filament pairs and around internal stops so gradients preserve the selected color order, pure endpoints, intended coverage, and smooth progression without unintended color jumps.

| ID | Proposal | Requirement | Expected result |
| --- | --- | --- | --- |
| GR-01 | 3.1 | Support ordered two-, three-, and four-filament gradients. | The gradient follows the selected filament order, progressing through adjacent filament pairs and retaining pure endpoints. |
| GR-02 | 3.2 | Provide editable color stops and transition midpoints. | Users control where colors appear and how transitions progress; dragging and numeric entry update the design consistently. |
| GR-03 | 3.3 | Support centered solid-color regions at internal stops. | Each internal color can hold a solid span whose width is constrained by neighboring transitions. |
| GR-04 | 3.4 | Provide layered and blended gradient-editor previews. | Users can inspect both component passes and predicted blended colors using the applicable color predictor. |
| GR-05 | 3.5 | Display gradients in Prepare before slicing. | Whole-model assignments and painted regions show the intended vertical direction, stops, transitions, and coverage. |
| GR-06 | 3.6 | Correct transitions between successive filament pairs and internal stops. | Two-, three-, and four-filament gradients avoid unintended pair changes, color jumps, and incorrect stop behavior. |

### FS migration - mixed color preview prediction feature

Integrate FullSpectrum's calibrated Kubelka–Munk/K-S prediction engine for supported two-, three-, and four-filament mixtures. Supply the measured bundle material profiles, stable material identities, and transmission-distance values needed to associate each supported filament with its calibration, including after filament slots are reordered. Materials outside the calibrated set must retain a usable fallback prediction path. Apply the same prediction rules across mixed-filament cards, recipe editing, gradient previews, and the Prepare model so a recipe is displayed consistently throughout the workflow. Connect color matching and recommended mixtures to that same predictor, allowing users to evaluate suggested recipes using the colors they will see when editing and previewing them.

| ID | Proposal | Requirement | Expected result |
| --- | --- | --- | --- |
| CP-01 | 1.1 | Integrate calibrated Kubelka–Munk/K-S color prediction. | Supported two-, three-, and four-filament bundle mixtures reproduce the FS reference prediction behavior. |
| CP-02 | 1.2 | Supply calibrated bundle material profiles and stable material identities. | Supported presets resolve to measured profiles and transmission-distance values; calibration follows the material when slots are reordered. Unsupported materials retain a usable prediction path. |
| CP-03 | 1.3 | Use consistent color prediction throughout the mixing workflow. | Mixed-filament cards, recipe editing, gradient previews, and Prepare use the same prediction rules for the same recipe and inputs. |
| CP-04 | 1.4 | Connect color matching and recommendations to the same predictor. | Suggested recipes are evaluated with the predictor used to display and edit them. |

### FS migration - UI/UX optimization

Improve control over painted color regions by adding surface-only painting, extra perimeters in painted zones, adjustable brush precision from 1× to 8×, and rectangle and polygon selection tools. Surface-only painting must support physical filaments, mixes, and gradients, deriving sidewall depth from wall count and extrusion widths while preserving the base material inside and respecting top and bottom shell settings. Extra painted perimeters must work consistently with this surface-depth behavior. Rectangle selection must support quick coverage of bounded patches, while polygon selection and higher brush precision provide control over detailed or irregular boundaries. Preserve painted assignments through slicing and project reopening, and prevent duplicate or overlapping wall paths when an area is repainted with its surrounding material.

| ID | Proposal | Requirement | Expected result |
| --- | --- | --- | --- |
| UX-01 | 4.1 | Provide Surface paint only for physical, mixed, and gradient facet painting. | Sidewall paint depth follows wall count and extrusion widths, with base material retained inside. Top/bottom coverage follows shell-layer settings. Whole-object and volume assignments are not shell-limited. |
| UX-02 | 4.2 | Allow extra perimeters in painted zones. | Users can reinforce the colored shell locally, with consistent behavior alongside Surface paint only. |
| UX-03 | 4.3 | Support brush precision from 1× to 8×. | Users can define finer painted boundaries that survive slicing and project reopening. |
| UX-04 | 4.4 | Provide rectangle painting selection. | Users can paint a bounded rectangular patch with a physical filament, mix, or gradient. |
| UX-05 | 4.5 | Provide polygon painting selection. | Users can define precise irregular boundaries with physical filaments, mixes, gradients, and surface controls. |
| UX-06 | 4.6 | Prevent duplicate walls when repainting with the surrounding material. | Same-material painting does not create overlapping wall paths, including when surface controls and extra painted perimeters are used. |

### FS migration - SML algorithm optimization

Simplify Subdivide Mix Layers (SML) so assigning a gradient automatically activates the successive Local-Z component passes needed to produce it. Separate gradient cycles from geometric layer height, using a 0.20 mm baseline cycle and automatically calculating component heights from recipe ratios and a default 0.06 mm minimum. Respect the active printer or extruder's maximum printable height, preserve pure-color endpoints, and account for ratios that printable limits cannot reproduce exactly. Apply full-domain handling automatically to object and volume assignments while keeping painted mixes within their boundaries. Preserve the configured initial-layer height and first-layer protection. Make direct multicolor the standard solver for eligible static recipes with three or more components, and enable independent Local-Z heights for eligible static mixes so their cycles can span geometric layers. Retain compatible handling of manual patterns, explicit A/B height overrides, and existing projects.

| ID | Proposal | Requirement | Expected result |
| --- | --- | --- | --- |
| SM-01 | 2.1 | Activate Local-Z subdivision automatically for gradients. | Assigning a gradient is sufficient; users need not enable global subdivision or full-domain overrides. |
| SM-02 | 2.2 | Separate gradient cadence from nominal process layer height. | A dedicated gradient cycle starts at 0.20 mm; changing geometric resolution does not require reconfiguring the gradient. |
| SM-03 | 2.3 | Calculate printable gradient component heights automatically. | Component heights follow recipe ratios and a default 0.06 mm minimum; pure endpoints remain pure and physical output reflects achievable ratios. |
| SM-04 | 2.4 | Use printer-defined maximum Local-Z pass heights. | The active filament/extruder's printable maximum replaces a separate global upper-height setting. |
| SM-05 | 2.5 | Respect object, volume, and painted assignment boundaries automatically. | Whole-object/volume mixes receive full-domain handling; painted mixes remain within painted regions and unrelated regions retain their behavior. |
| SM-06 | 2.6 | Preserve configured initial-layer height and protection. | The first object layer follows Initial layer height independently of nominal process height; any permitted subdivision must preserve its total thickness. |
| SM-07 | 2.7 | Make direct multicolor SML the normal solver for eligible static mixes. | Static recipes with three or more active components receive direct component passes once SML is enabled, respecting printable limits and accumulated proportions. |
| SM-08 | 2.8 | Support independent Local-Z heights for eligible static mixes. | Mix cycles can span nominal layers and allocate heights by recipe weight and printer limits; gradients, manual patterns, and explicit A/B overrides retain their appropriate handling. |

### FS migration - Xipit UI rework integration

Integrate Xipit's community UI rework and the associated Snorca adaptations to make filament selection, mixed-recipe inspection, and painted-material identification easier. Display static mixes as component-color blocks with filament numbers and percentages on hover, gradients as continuous strips with numbered stops, and manual patterns as ordered filament strips. Retain numeric mix badges, compatibility warnings, tooltips, and access to existing editing actions. Add a filament-family selection step to the Snapmaker palette, resolve a compatible printer/nozzle profile, and let users choose an official swatch or move between the palette and custom-color dialog with clear Back and Cancel behavior. Provide an Identify color tool that reports the material or recipe under the pointer, including subdivided painted facets and inherited object/part assignments, while preserving painting, camera navigation, and section-view controls. Keep the new controls readable across themes, display scaling, and constrained panel sizes.

| ID | Source | Requirement | Expected result |
| --- | --- | --- | --- |
| XR-01 | `0ba1d67f11`: sidebar recipe preview | Show static mixtures as component-color blocks. | Users see the contributing filament numbers and relative proportions in the sidebar, with exact percentages on hover. Small components remain visible through a minimum display width. |
| XR-02 | `0ba1d67f11`: sidebar gradient preview | Show gradients as continuous color strips with numbered stops. | Users can recognize the gradient direction, component order, and stop placement directly in the mixed-filament list. The strip uses the shared gradient prediction. |
| XR-03 | `0ba1d67f11`: sidebar pattern preview | Show manual recipes as ordered filament strips. | Users can inspect the first manual-pattern group's sequence using numbered color blocks, with an overflow fade when the strip cannot fit the sequence. Full recipe details remain available through the existing tooltip/editor. |
| XR-04 | `0ba1d67f11`: sidebar integration | Preserve recipe identity and existing actions in the redesigned rows. | Numeric mixed-filament badges, recipe tooltips, compatibility warnings, click-to-edit, and existing options remain available alongside the visual previews. |
| XR-05 | `0ba1d67f11`: family/color dialogs | Add filament-family navigation to the Snapmaker palette. | Users choose a family from a scrollable tile list and then choose a color from its catalog. The current family is highlighted, swatch selection applies directly, and Back returns to family selection. |
| XR-06 | `0ba1d67f11`: family preset resolution | Apply a compatible profile when a palette family is selected. | Family selection respects the active printer/nozzle, preserves the current compatible profile when staying in its family, and disables families with no compatible profile. Browsing or cancelling does not change the project's filament assignment. |
| XR-07 | `0ba1d67f11`: classic/palette navigation | Connect official palette colors and custom-color selection. | Other Colors opens the classic picker, and Snapmaker palette provides a return route. Saved custom swatches remain available, and cancellation leaves the project selection unchanged. |
| XR-08 | `0ba1d67f11`: Identify color toolbar tool | Identify the filament or mixed recipe under the pointer. | Hovering over the model shows a color swatch, the numeric filament/mix identity, and the physical profile or recipe details: static percentages, gradient component order, or manual-pattern groups. |
| XR-09 | `0ba1d67f11`: facet lookup and input handling | Make color identification precise and non-destructive. | Inspection resolves subdivided painted facets and falls back to object/part assignments on unpainted surfaces. It does not paint or create undo snapshots, and continues to work with camera movement and section clipping. |
| XR-10 | `0ba1d67f11`: presentation integration | Keep the new controls readable and usable across layouts. | Preview labels contrast with their swatches; very light/dark swatches retain visible boundaries; crowded labels are handled cleanly. Family lists scroll, and palette placement and sizing account for display scaling and available screen space. |

The new sidebar strips present existing recipes; they do not redefine gradient evaluation or SML planning. The same commit also corrects measured bundle picker colors and shared calibration/fallback routing. Those supporting prediction changes extend CP-02–CP-04 and are not counted again as UI-rework requirements.

### Shared requirements included in the five categories

- Save and restore material identity, calibration settings, recipes, gradient order/stops/midpoints/solid spans, solver/height settings, painted boundaries, assignments, and surface settings. Preserve material references through slot reordering.
- Preserve compatible behavior for older projects, including manual patterns and explicit A/B cadence heights, when introducing the new defaults.
- Propagate edits through relevant previews, slicing, physical tool selection, and wipe-tower output.
- Retain the applicable Xipit / Hanno Witzleb gradient-interface and Neotko painting/surface-work attribution.
- Validate the integrated workflow through regression checks and representative shipping-bundle prints. Record toolchange count, print time, and material use where relevant; the proposal does not establish a guaranteed saving.

### Implementation decisions to capture in the retrospective

| Topic | Recorded implementation outcome |
| --- | --- |
| Calibrated predictor | Supported bundle materials select calibrated prediction automatically. A separate process checkbox is removed; unsupported automatic preview combinations use FilamentMixer. Stable IDs, rather than display color alone, identify calibrated materials. |
| Independent static-mix heights | The proposal left the default open. The port enables independent heights with SML and hides the separate switch. Eligible two-component static mixes are included in the independent-height path. |
| Direct multicolor | Enabling SML restores the direct solver, including projects with old hidden flags saved as false. Explicit A/B overrides retain precedence. |
| First layer | Protection is enforced and the separate switch is hidden. The first layer remains unsplit at its configured initial height, including when a legacy project saved protection as false. |
| Solid-color widths | Internal colors have independent width handles and numeric entry. New gradients start at 3% per internal color; older recipes retain their saved legacy width behavior. |
| Design versus physical previews | Blended design previews follow the requested transition. Layered previews and slicing account for printable pass heights. A blended design color is not a guarantee of an achievable physical ratio at every position. |
| Solid-zone transition ramp | The final implementation smooths entry to and exit from internal solid-color regions by increasing the relevant component's pass height within printer limits. The 0.20 mm setting is the baseline gradient cycle; the ramp can extend the local cycle. |
| Surface controls | Surface-only painting defaults off; extra painted walls default to zero and are visible/applied only while surface-only painting is enabled. Stored values are retained when hidden. |
| Same-material overlap | Snorca's existing region deduplication is retained. This is compatibility work, rather than a newly introduced deduplication algorithm. |

## 2. Specific tasks for the In development phase

Task titles use action verbs so they can be pasted directly into a tracker. All rows have the retrospective completion status defined above. The last column describes the implementation scope and expected completion result, rather than claiming a new acceptance test was run while preparing this document.

### FS migration - gradient color logic optimization

| Task ID and title | Requirements | Scope / completion result |
| --- | --- | --- |
| G-D01 — Implement ordered multi-filament gradient evaluation | GR-01 | Resolve two-to-four-filament sequences through adjacent pairs; preserve user order, direction, and pure endpoints when recipes are normalized. |
| G-D02 — Add stop and midpoint editing controls | GR-02 | Implement draggable stops/midpoints, precise position entry, and minimum stop-gap handling; propagate edits to preview and slicing inputs. |
| G-D03 — Add independent solid-color width controls | GR-03 | Provide centered per-color spans, paired width handles, and numeric entry; constrain widths at adjacent transitions and initialize new internal stops at 3%. |
| G-D04 — Integrate layered and blended editor previews | GR-04, CP-03 | Render component-pass and blended views from the shared recipe. Apply the common predictor and reflect physical pass spacing in the layered view. |
| G-D05 — Render gradient assignments in Prepare | GR-05 | Apply vertical gradient rendering to whole objects and painted facets; update direction, stop positions, and region coverage when the recipe changes. |
| G-D06 — Correct pair-boundary and internal-stop transitions | GR-01, GR-06 | Align adjacent-pair evaluation around stops and solid spans; remove unintended transition jumps and broad constant-color bands caused by minimum-height clamping of design previews. |
| G-D07 — Smooth physical transitions into solid-color regions | GR-03, GR-06, SM-04 | Add the Local-Z height ramp on both sides of internal solid spans; preserve the fading component, respect per-filament maxima, and share the behavior with the layered preview. Include moved-stop and reduced-maximum regressions. |
| G-D08 — Preserve gradient definitions across project reloads | GR-01–GR-03; shared persistence | Serialize filament order, stops, midpoints, and per-color widths; read legacy recipes without explicit widths and verify that typed recipe normalization does not change traversal order. |

Implementation references: [gradient evaluation and preview](../src/libslic3r/MixedFilament/GradientPreview.cpp), [gradient selector](../src/slic3r/GUI/MixedGradientSelector.cpp), [recipe dialog](../src/slic3r/GUI/MixedFilamentDialog.cpp), [Prepare rendering](../src/slic3r/GUI/3DScene.cpp), and [product-port regressions](../tests/libslic3r/test_product_port.cpp). Commit `2740cbd425` records the solid-zone ramp.

### FS migration - mixed color preview prediction feature

| Task ID and title | Requirements | Scope / completion result |
| --- | --- | --- |
| C-D01 — Port the calibrated KM/K-S prediction engine | CP-01 | Integrate measured absorption/scattering profiles and mixture corrections for supported two-, three-, and four-component recipes; preserve FS calibration coefficients and reference behavior. |
| C-D02 — Map shipping-bundle presets to calibrated materials | CP-02 | Register stable cyan, magenta, yellow, and gray SKU aliases, transmission distances, and picker colors; correct matching older catalogs and avoid assigning calibration aliases to unsupported materials. |
| C-D03 — Unify automatic color-prediction routing | CP-03 | Route cards/badges, recipe editing, gradient views, and Prepare through a common display context. Automatically choose calibrated prediction for supported combinations and propagate material changes consistently. |
| C-D04 — Use calibrated prediction for matching and recommendations | CP-04 | Evaluate candidate mixtures with the same material identity and prediction context used by recipe previews; retain the existing matching and recommended-mixture workflows. |
| C-D05 — Preserve useful fallback colors and correct endpoint fringes | CP-02, CP-03 | Use FilamentMixer for unsupported automatic preview combinations; anchor its displayed mixture endpoints to the actual input colors, preserve identical-color mixes, and cover the reported green-to-white fringe. |
| C-D06 — Persist calibration identity through slot changes | CP-02, CP-03; shared persistence | Save material IDs, transmission distances, and compatible color-engine settings; realign material references when slots move and verify that display-color edits do not silently replace material identity. |

Implementation references: [material library](../src/libslic3r/FilamentColorLibrary.cpp), [calibrated model](../src/libslic3r/FullSpectrumKSPairResidual.cpp), [display prediction](../src/libslic3r/MixedFilament/Display.cpp), [matching integration](../src/slic3r/GUI/MixedColorMatchHelpers.cpp), [calibration regressions](../tests/libslic3r/test_full_spectrum_ks_pair_residual.cpp), and [endpoint-correction investigation](FilamentMixer-Endpoint-Correction.md).

### FS migration - UI/UX optimization

| Task ID and title | Requirements | Scope / completion result |
| --- | --- | --- |
| U-D01 — Implement surface-limited facet painting | UX-01 | Derive sidewall depth from wall count and widths; retain base material inside, honor top/bottom shell settings, and keep whole-object/volume assignments outside the facet-only restriction. |
| U-D02 — Integrate extra walls in painted zones | UX-02 | Apply additional perimeters locally with surface-only painting; hide and deactivate the control when surface-only painting is off while retaining its stored count. |
| U-D03 — Add adjustable brush precision | UX-03 | Expose the 1×–8× precision range and connect it to facet subdivision/selection so detailed painted boundaries remain available to slicing. |
| U-D04 — Add rectangle painting selection | UX-04 | Implement projected rectangle selection and assign physical, mixed, or gradient materials within the selected screen region. |
| U-D05 — Add polygon painting selection | UX-05 | Implement polygon-mask interaction and projected irregular-region selection; apply the selected material with the existing surface controls. |
| U-D06 — Preserve same-material painting deduplication | UX-06 | Retain Snorca's region-merging behavior when the painted material matches its surroundings, alongside the added surface-depth and extra-perimeter processing. |
| U-D07 — Simplify and stabilize color-mixing settings | SM-01–SM-08, UX-02; shared UI integration | Group subdivision controls before surface controls; hide redundant default switches and show dependent fields conditionally. Preserve double precision during sidebar synchronization so unchanged values do not reappear as modified. Retain theme/DPI behavior in the integrated editor. |
| U-D08 — Preserve painted assignments through project round trips | UX-01–UX-06; shared persistence | Keep triangle annotations, object/volume assignments, surface settings, and mixed-material references intact through the production 3MF save/reopen path; retain the applicable painting and editor attribution. |

Implementation references: [painting segmentation](../src/libslic3r/MultiMaterialSegmentation.cpp), [triangle selection](../src/libslic3r/TriangleSelector.cpp), [painting tools](../src/slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.cpp), [conditional settings](../src/slic3r/GUI/ConfigManipulation.cpp), [settings panel](../src/slic3r/GUI/Tab.cpp), and [port implementation notes](FullSpectrum-Product-Port.md).

### FS migration - SML algorithm optimization

| Task ID and title | Requirements | Scope / completion result |
| --- | --- | --- |
| S-D01 — Activate subdivision from gradient assignments | SM-01 | Detect assigned gradients and enable their Local-Z planning without requiring global SML or full-domain switches; retain normal slicing for unrelated ordinary objects. |
| S-D02 — Decouple gradient cycles from geometric layers | SM-02 | Carry the independent gradient cadence across nominal layer boundaries with a 0.20 mm baseline; preserve the gradient setup when nominal height changes. |
| S-D03 — Enforce printable component-pass heights | SM-03, SM-04 | Derive component heights from ratios and the 0.06 mm default minimum; keep pure endpoints pure, clamp infeasible ratios, and split oversized contributions using the active printer/extruder maximum. |
| S-D04 — Apply automatic full-domain and painted-region planning | SM-05 | Activate full-domain processing for eligible object/volume mixes, constrain painted mixes to their masks, and maintain ordinary physical-material behavior outside mixed regions. |
| S-D05 — Enforce initial-layer alignment and protection | SM-06 | Keep the first layer unsplit at Initial layer height; normalize legacy disabled protection values and ensure nominal height cannot replace the configured first-layer thickness. |
| S-D06 — Integrate the direct multicolor solver | SM-07 | Allocate static three- and four-component recipes directly to physical passes. Retain the nominal-budget path and carried component thickness where independent heights are not applicable; preserve manual-pattern and explicit A/B exceptions. |
| S-D07 — Enable independent heights for static SML mixes | SM-08 | Allocate component heights from weights and minimum height across nominal-layer boundaries, including eligible two-component recipes; split large contributions and handle short-region endings. |
| S-D08 — Normalize SML defaults while preserving legacy recipes | SM-07, SM-08; shared compatibility | Restore direct/independent behavior when SML is enabled, including saved false hidden flags. Use the typed recipe model with legacy adapters while retaining A/B precedence, manual sequences, stable IDs, unavailable rows, and existing serialization. |
| S-D09 — Correct tool changes at mixed-region boundaries | SM-05, SM-07, SM-08; shared G-code integration | Honor required physical-filament changes even above the last planned wipe-tower layer or on skipped sparse layers; return to the object's material after a painted Local-Z region ends. |
| S-D10 — Add integrated slicing regressions and build validation | All four categories; shared integration | Cover automatic gradients, surface-painted gradients, static multicolor/independent passes, ordinary control objects, configuration compatibility, and project persistence. Check generated tool use and finite coordinates/heights; build the Windows tests, application DLL, and launcher. |

Implementation references: [Local-Z planner](../src/libslic3r/PrintObjectSlice.cpp), [print configuration](../src/libslic3r/PrintConfig.cpp), [G-code integration](../src/libslic3r/GCode.cpp), [typed recipe manager](../src/libslic3r/MixedFilament/Manager.cpp), [refactor and compatibility record](Mixed-Filament-Refactor.md), and [product-port regressions](../tests/libslic3r/test_product_port.cpp).

### FS migration - Xipit UI rework integration

| Task ID and title | Requirements | Scope / completion result |
| --- | --- | --- |
| X-D01 — Integrate static-mix recipe cards | XR-01 | Adapt the community recipe-card presentation into a sidebar preview widget; draw component-color blocks with readable minimum widths, filament-number labels, and percentage labels on hover. |
| X-D02 — Add gradient strips and stop labels to sidebar rows | XR-02 | Sample the shared gradient color function across the strip and position numeric component labels at the saved stops; manage label spacing when the sidebar is narrow. |
| X-D03 — Add manual-pattern sequence strips | XR-03 | Render the first pattern group's ordered component IDs as colored blocks and fade overflowing content; keep the existing recipe details accessible. |
| X-D04 — Preserve sidebar editing and compatibility feedback | XR-04 | Replace the text-only recipe area with the new preview while retaining numeric badges, tooltips, compatibility errors, edit callbacks, and existing options. Store the preview's own display data so list rebuilds cannot invalidate it. |
| X-D05 — Add the filament-family palette flow | XR-05 | Build the scrollable family tile dialog, current-family highlighting, family-to-swatch navigation, immediate swatch selection, and Back/Cancel actions using the existing official-color catalog. |
| X-D06 — Resolve and apply compatible family profiles | XR-06 | Match family profiles against printer/nozzle compatibility, retain current-profile edits when appropriate, and disable unavailable families. Apply the selected profile through the normal preset-update path only after a color is chosen; add family-resolution regression coverage. |
| X-D07 — Connect the palette and classic color picker | XR-07 | Add the Snapmaker palette route to the classic picker and Other Colors to the palette flow; preserve custom swatches and close each modal before switching dialogs. Keep project changes tied to a completed selection. |
| X-D08 — Add the Identify color tool and recipe tooltip | XR-08 | Add the painting-toolbar inspection action and hover tooltip with color, numeric identity, physical preset, and static/gradient/manual recipe details. |
| X-D09 — Resolve inspected paint accurately without editing it | XR-09 | Query the actual subdivided facet state, resolve unpainted object/part material, refresh hit detection after camera/clipping changes, and keep identification outside painting/undo paths. Add lookup regressions for split facets, restored assignments, remapped IDs, and invalid hits. |
| X-D10 — Adapt preview and palette presentation to theme and scale | XR-10 | Apply contrasting labels, light/dark swatch borders, hover feedback, narrow-strip label handling, scrollable family tiles, and DPI-aware palette shape, placement, and sizing. |

Implementation references at UI integration commit `0ba1d67f11`: [recipe preview widget](../src/slic3r/GUI/MixedFilamentRecipePreview.cpp), [sidebar integration](../src/slic3r/GUI/Plater.cpp), [palette dialogs](../src/slic3r/GUI/FilamentColorDialog.cpp), [preset selection flow](../src/slic3r/GUI/PresetComboBoxes.cpp), [family resolution](../src/libslic3r/FilamentColorLibrary.cpp), [Identify color UI](../src/slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.cpp), [inspection input handling](../src/slic3r/GUI/Gizmos/GLGizmoPainterBase.cpp), [facet lookup](../src/libslic3r/TriangleSelector.cpp), [palette regressions](../tests/libslic3r/test_filament_palette.cpp), and [facet identification regressions](../tests/libslic3r/test_triangle_selector.cpp).

## 3. Completion evidence and release acceptance

The implementation history contains the selected feature port (`687192c8ce`), the compatible mixed-filament refactor (`7cb1783d1b`), and the solid-zone transition ramp (`2740cbd425`). The repository records Windows builds, focused native-widget checks, slicing/G-code checks, and production 3MF round trips. The refactor record reports 243 regression cases: 241 passed and two pre-existing expected failures. That count predates the later ramp commit; it is not a newly verified result for the current head.

This document was checked against the proposal, implementation notes, relevant source, and test definitions. No application build, automated suite, GUI walkthrough, or physical print was rerun while preparing it. Owners, historical dates, and effort estimates are intentionally left for the tracker records rather than inferred from commit dates.

The UI-rework extension was reviewed from committed snapshots with `git show` and branch comparisons, without switching branches or changing application code. Its implementation and focused regression definitions are present in `0ba1d67f11`; no new execution result or full UI acceptance is claimed here. The original 243-case historical result above is not validation of this later UI commit. Only this planning document was edited for the extension.

The following acceptance evidence remains to be attached or confirmed according to the existing repository notes. It does not change the retrospective development status above.

| Acceptance item | Category ownership | Evidence to attach / confirm |
| --- | --- | --- |
| Shipping-bundle color validation | Color prediction | Representative integrated Snorca prints using the actual shipping materials and print conditions. The proposal's 0.70 versus 16.53 mean ΔE00 values come from 173 existing calibration swatches, not an independent shipping-bundle test. |
| Integrated editor and painting walkthrough | Gradient; UI/UX | Full application coverage of editing, Prepare updates, brush/rectangle/polygon painting, and save/reopen behavior. Existing notes record focused widget checks and a remaining full walkthrough. |
| UI-rework interaction walkthrough | Xipit UI rework integration | Attach or confirm coverage for static/gradient/manual sidebar cards, hover percentages, edit/options access, family compatibility, Back/Cancel and custom-color navigation, Identify color on split/unpainted facets, camera/section controls, themes, and display scaling. |
| Physical gradient and SML validation | Gradient; SML | Representative two-/three-/four-component cases, unequal/extreme ratios, first layers, short painted regions, transitions to physical filaments, tool ordering, and wipe-tower behavior. Record toolchanges and time before making efficiency claims. |
| Surface-painting interactions and impact | UI/UX; SML | Surface depth, top/bottom shells, extra painted walls, same-material repainting, and beam-interlocking interaction; compare material use and print time where relevant. Beam interlocking can bypass the normal segmentation width limit and needs explicit acceptance coverage. |
| End-to-end proposal scenario | All four categories | Create a calibrated three-filament gradient, edit stops, paint one face with surface-only mode, slice at two nominal heights without changing gradient setup, verify base-material interior and legal passes, save/reopen, and validate a representative print. |

Scope remains the selected product port. The implementation record excludes FS image mapping, perimeter modulation, component-bias controls, the swatch generator, a second infill-subdivision option, and the FS vNext 3MF package format.
