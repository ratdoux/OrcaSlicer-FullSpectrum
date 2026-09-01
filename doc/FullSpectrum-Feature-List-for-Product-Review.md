# FullSpectrum Feature Summary

_Local-Z, gradients, and surface painting_

These five improvements reduce mixed-filament setup friction, make layer-height behavior more predictable, and give users more precise control over visible surface results.

## 1. First-layer height alignment

**What changed:** In SML / Local-Z workflows, the first object layer follows the configured **Initial layer height**. If the layer is subdivided, its combined passes still equal that height.

**Product value:** Predictable first-layer thickness and adhesion across standard and mixed-filament prints.

## 2. Printer-defined Local-Z ceiling

**What changed:** The separate Local-Z upper-height setting has been removed. The upper bound now comes from the printer's maximum layer height for the active filament or extruder.

**Product value:** Fewer settings to manage and one authoritative, printer-aware safety limit.

## 3. Independent gradient cadence

**What changed:** Gradients use Local-Z automatically and have a dedicated **Gradient Local-Z layer height**. Users no longer need to increase the model's process layer height or enable SML just to activate a gradient.

**Product value:** Geometry quality and color-transition cadence can be tuned independently, with less setup friction.

## 4. Surface depth for mixed filaments

**What changed:** A surface-only option controls how deeply facet-painted physical, mixed, or gradient filament regions extend into the wall shell instead of recoloring the full volume.

**Product value:** More predictable coloration, less internal material mixing, and better control of appearance versus material use.

## 5. Rectangle and polygon painting

**What changed:** Rectangle and polygon selection tools complement brush painting. Rectangle covers large areas quickly, while polygon creates precise, editable boundaries around irregular regions.

**Product value:** Faster painting of large faces and cleaner control on complex shapes.

## 6. Gradient preview in the Prepare tab

**What changed:** Models and painted regions assigned a gradient now display the vertical color transition directly in the **Prepare** tab. The preview reflects the gradient's component colors and stop positions on the model before slicing.

**Product value:** Users can evaluate gradient direction, transitions, and coverage earlier, increasing confidence in the result and reducing trial-and-error iterations.

## 7. Direct multicolor Local-Z

**What changed:** An experimental direct solver can represent static, non-gradient recipes containing three or more physical filaments by allocating Local-Z passes directly across all active components instead of reducing the recipe to a sequence of two-filament pairs. It respects the minimum sublayer height and carries any unrepresented ratio error into subsequent layers.

**Product value:** Complex multicolor recipes can reproduce their intended aggregate mixture more faithfully across the print while keeping every generated pass within legal layer-height limits.

---

**Product story:** Printer limits define safe behavior, complex multicolor mixes gain more direct representation, gradients work independently and remain visible during preparation, and surface painting gains both depth and selection control.
