#include "Scheduler.hpp"

#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <map>
#include <stdexcept>

namespace Slic3r::MaterialBand {

std::string diagnostic_gcode(const Schedule& schedule, bool relative_extrusion, bool reset_each_path)
{
    GCodeWriter writer;
    writer.apply_print_config(static_cast<const PrintConfig&>(FullPrintConfig::defaults()));
    writer.config.gcode_flavor.value             = gcfRepRapFirmware;
    writer.config.use_relative_e_distances.value = relative_extrusion;
    writer.config.filament_diameter.values.assign(schedule.machine.filament_diameter_mm.begin(),
                                                  schedule.machine.filament_diameter_mm.begin() + schedule.machine.material_count);
    writer.config.retraction_length.values.assign(schedule.machine.material_count, 1.);
    writer.config.retraction_speed.values.assign(schedule.machine.material_count, 30.);
    writer.config.deretraction_speed.values.assign(schedule.machine.material_count, 30.);
    writer.config.retract_restart_extra.values.assign(schedule.machine.material_count, 0.);
    writer.config.use_firmware_retraction.value        = false;
    writer.config.single_extruder_multi_material.value = schedule.machine.shared_nozzle;
    writer.config.travel_speed_z.value                 = schedule.machine.max_z_mm_s;
    std::vector<unsigned int> extruders;
    for (unsigned int i = 0; i < schedule.machine.material_count; ++i)
        extruders.push_back(i);
    writer.set_extruders(extruders);
    writer.set_is_first_layer(false);
    writer.set_position(schedule.initial_position);
    const auto travel = [&](const Vec3d& to, const char* comment) {
        const Vec3d from = writer.get_position();
        if ((from.head<2>() - to.head<2>()).norm() <= 1e-12)
            return writer.travel_to_z(to.z(), comment, true);
        if (std::abs(from.z() - to.z()) > 1e-12)
            throw std::invalid_argument("Scheduled diagnostic travel must separate XY and Z");
        return writer.travel_to_xy(to.head<2>(), comment);
    };
    std::map<OperationRef, const BandPath*> paths;
    for (const auto& object : schedule.objects)
        for (const auto& path : object.paths->paths)
            paths.emplace(OperationRef{object.object, path.id}, &path);
    std::string output = "; MaterialBand scheduled diagnostic only; not a production FinalPrintPlan\n";
    output += ";MB_INITIAL_POSITION X" + std::to_string(schedule.initial_position.x()) + " Y" +
              std::to_string(schedule.initial_position.y()) + " Z" + std::to_string(schedule.initial_position.z()) + "\nG90\n";
    output += relative_extrusion ? "M83\n" : "M82\n";
    uint64_t operation = 0;
    for (const auto& item : schedule.order) {
        const auto& path = *paths.at(item.operation);
        output += ";MB_OPERATION TRAVEL\n";
        if (writer.extruder())
            output += writer.retract();
        // Tool changes have no opaque motion in this diagnostic profile. They
        // occur after the first (lift) move, before descent to the next bead.
        for (size_t j = 1; j < item.travel.size(); ++j) {
            output += travel(item.travel[j], "scheduled travel");
            if (j == 1)
                output += writer.set_extruder(path.band.material.value - 1);
        }
        if (item.travel.size() <= 1)
            output += writer.set_extruder(path.band.material.value - 1);
        if (reset_each_path)
            output += writer.reset_e(true);
        output += writer.unretract();
        output += ";MB_CYCLE " + std::to_string(path.band.macro_cycle) + " INTERFACE " + std::to_string(path.shared_interface) + " PATH " +
                  std::to_string(path.id) + " MATERIAL " + std::to_string(path.band.material.value) + " OBJECT " +
                  std::to_string(item.operation.object) + " OPERATION " + std::to_string(++operation) + "\n";
        output += ";MB_OPERATION DEPOSITION\n";
        const double diameter      = schedule.machine.filament_diameter_mm.at(path.band.material.value - 1);
        const double filament_area = PI * diameter * diameter / 4.;
        for (size_t j = 0; j < path.segments.size(); ++j) {
            const auto& segment = path.segments[j];
            output += ";MB_BEAD WIDTH " + std::to_string(segment.width_mm) + " HEIGHT_START " + std::to_string(segment.height_start_mm) +
                      " HEIGHT_END " + std::to_string(segment.height_end_mm) + "\n";
            output += writer.set_speed(segment.speed_mm_s * 60.);
            output += writer.extrude_to_xyz(path.vertices[j + 1], segment.volume_mm3 / filament_area);
        }
    }
    output += ";MB_OPERATION TRAVEL\n";
    if (writer.extruder())
        output += writer.retract();
    for (size_t j = 1; j < schedule.final_travel.size(); ++j)
        output += travel(schedule.final_travel[j], "scheduled final lift");
    return output;
}
} // namespace Slic3r::MaterialBand
