#ifndef slic3r_LocalZBridge_hpp_
#define slic3r_LocalZBridge_hpp_

#include "AABBTreeLines.hpp"
#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"

#include <utility>
#include <vector>

namespace Slic3r {

// Continuous mix bridges let neighboring Local-Z zones that share physical
// filaments print each shared filament as one connected layer. A zone is a
// mixed region or a single-filament region. Planning gives every zone the same
// ordered slots, each printing one filament; a zone without a slot's filament
// gives it zero height at that point of its stack.
//
// Inside a zone the slots keep that zone's interfaces. Across a band at a
// boundary between zones that share a filament the interfaces blend, so one
// filament's layer thins while its neighbor's thickens. A layer that only one
// side has tapers to nothing inside the band, filling the space the other
// layers leave. Between two mixes the band is centered on the boundary. A
// single-filament zone absorbs the whole band, so a mix keeps its exact ratio
// up to its boundary, for example a painted shell over the object's body.
//
// The field only answers geometry queries. Planning decides which zones bridge
// and G-code export decides how a pass is emitted.
class LocalZBridgeField
{
public:
    struct Row
    {
        ExPolygons          masks;
        // Absolute Z of every slot interface, bottom to top: slot s spans
        // [bounds[s], bounds[s + 1]]. A slot this zone does not print has
        // equal bounds. Zones may start and end at different heights.
        std::vector<double> bounds;
        // A single-filament zone takes the whole transition band on its side.
        bool                absorbs_transition{false};
    };

    struct Span
    {
        double lower{0.};
        double upper{0.};
        double height() const { return upper - lower; }
    };

    LocalZBridgeField(double transition_width_mm, std::vector<unsigned int> slot_filaments, std::vector<Row> rows);

    size_t                           slot_count() const { return m_slot_filaments.size(); }
    const std::vector<unsigned int>& slot_filaments() const { return m_slot_filaments; }
    const std::vector<Row>&          rows() const { return m_rows; }

    // Normalized influence of each row at a point in object coordinates. Only
    // the owning row and rows sharing a filament with it contribute.
    std::vector<double> row_weights_at(const Point& point) const;
    // Slot heights at a point.
    std::vector<double> heights_at(const Point& point) const;
    // Lower and upper Z of one slot at a point; empty when the slot is not printed there.
    Span slot_span_at(const Point& point, size_t slot_index) const;
    // Lowest bottom and highest top of a slot over every member row. Blended
    // spans always lie inside these bounds, so a pass can use the top as its
    // nominal plane and keep ordering and travel above its material.
    Span   slot_bounds(size_t slot_index) const;
    double slot_max_height(size_t slot_index) const;
    // Whether any member row prints the slot.
    bool   slot_used(size_t slot_index) const;
    // Outline where the slot is printed, clipped to `outline`: every zone that
    // prints it, plus the part of each sharing neighbor its layer reaches into.
    ExPolygons slot_island(size_t slot_index, const ExPolygons& outline) const;

    // Splits a path into segments whose Z and bead height follow the field and
    // drops the parts where the slot is not printed. Runs that stay at one
    // height keep their source vertices; only the transition band is
    // resampled. Returns nothing for an invalid path or slot index.
    std::vector<ExtrusionPathSloped> modulate_path(const ExtrusionPath& path, size_t slot_index, double z_offset) const;

private:
    // Blended slot interfaces at a point.
    std::vector<double> stack_at(const Point& point) const;
    double              row_height(const Row& row, size_t slot_index) const { return row.bounds[slot_index + 1] - row.bounds[slot_index]; }
    // How far the transition band of `from` reaches into a sharing neighbor.
    double              band_reach_mm(const Row& from, const Row& into) const;

    double                                           m_half_width_mm{0.};
    std::vector<unsigned int>                        m_slot_filaments;
    std::vector<Row>                                 m_rows;
    // m_shares[r][q]: rows r and q print at least one common filament.
    std::vector<std::vector<bool>>                   m_shares;
    std::vector<AABBTreeLines::LinesDistancer<Line>> m_distancers;
};

} // namespace Slic3r

#endif
