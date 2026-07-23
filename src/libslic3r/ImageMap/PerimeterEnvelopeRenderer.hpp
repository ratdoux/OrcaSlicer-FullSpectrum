#ifndef slic3r_ImageMap_PerimeterEnvelopeRenderer_hpp_
#define slic3r_ImageMap_PerimeterEnvelopeRenderer_hpp_

#include "../ExPolygon.hpp"

#include <cstdint>
#include <memory>

namespace Slic3r {

class Layer;
class ModelObject;
class PrintObject;

namespace ImageMap {

bool model_has_perimeter_modulation(const ModelObject& model_object);
bool model_uses_perimeter_modulation_filament(const ModelObject& model_object,
                                              uint64_t           mixed_filament_stable_id,
                                              unsigned int       fallback_filament_id);

// Immutable slice-time renderer. Construction snapshots all mesh/source
// references and transforms; apply() only mutates the supplied layer.
class PerimeterEnvelopeRenderer
{
public:
    static std::unique_ptr<PerimeterEnvelopeRenderer> create(const PrintObject& print_object);

    ~PerimeterEnvelopeRenderer();
    PerimeterEnvelopeRenderer(PerimeterEnvelopeRenderer&&) noexcept;
    PerimeterEnvelopeRenderer& operator=(PerimeterEnvelopeRenderer&&) noexcept;
    PerimeterEnvelopeRenderer(const PerimeterEnvelopeRenderer&)            = delete;
    PerimeterEnvelopeRenderer& operator=(const PerimeterEnvelopeRenderer&) = delete;

    bool apply(Layer& layer) const;

    // Orca retains an uncompensated first-layer envelope alongside the
    // compensated region surfaces. Keep alternate pipeline envelopes on the
    // same sampled image-map boundary without exposing texture concerns to slicing.
    bool apply_to_envelope(ExPolygons& envelope, const Layer& layer) const;

private:
    struct Impl;
    explicit PerimeterEnvelopeRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace ImageMap
} // namespace Slic3r

#endif
