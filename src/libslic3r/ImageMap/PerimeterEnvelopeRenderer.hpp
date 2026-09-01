#ifndef slic3r_ImageMap_PerimeterEnvelopeRenderer_hpp_
#define slic3r_ImageMap_PerimeterEnvelopeRenderer_hpp_

#include "../ExPolygon.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace Slic3r {

class Layer;
class ModelObject;
class MixedFilamentManager;
class PrintObject;
namespace ImageMap {

bool                 model_has_perimeter_modulation(const ModelObject& model_object);
bool                 model_uses_perimeter_modulation_filament(const ModelObject& model_object,
                                                              uint64_t           mixed_filament_stable_id,
                                                              unsigned int       fallback_filament_id);
// Returns the one-based physical filament selected by the highest-priority
// Simple PM image that requests whole-object cadence synchronization.
std::optional<unsigned int> model_whole_object_cadence_filament(const ModelObject&          model_object,
                                                                const MixedFilamentManager& manager,
                                                                size_t                      num_physical,
                                                                int                         layer_index,
                                                                float                       layer_print_z,
                                                                float                       layer_height);

// Immutable slice-time renderer. Construction snapshots all mesh/source
// references and transforms; application modulates the material envelope
// before walls are generated.
class PerimeterEnvelopeRenderer
{
public:
    static std::unique_ptr<PerimeterEnvelopeRenderer> create(const PrintObject& print_object);

    ~PerimeterEnvelopeRenderer();
    PerimeterEnvelopeRenderer(PerimeterEnvelopeRenderer&&) noexcept;
    PerimeterEnvelopeRenderer& operator=(PerimeterEnvelopeRenderer&&) noexcept;
    PerimeterEnvelopeRenderer(const PerimeterEnvelopeRenderer&)            = delete;
    PerimeterEnvelopeRenderer& operator=(const PerimeterEnvelopeRenderer&) = delete;

    // Perimeter path modulation V2: reshape the slice boundary first, then let
    // the normal perimeter generator derive every wall from that geometry.
    // This prevents the external wall from being shifted onto an already
    // generated inner wall.
    bool apply_to_slices(Layer& layer) const;

    // Hybrid mode performs its width contribution after ordinary wall
    // topology exists. Adaptive ownership is established earlier by material
    // segmentation; this pass only modulates each already-owned outer wall and
    // never recolors fragments of a generated path.
    bool apply_to_perimeters(Layer& layer) const;

private:
    struct Impl;
    explicit PerimeterEnvelopeRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace ImageMap
} // namespace Slic3r

#endif
