#include "SnapmakerU1.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCodeWriter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Slic3r::MaterialBand {
namespace {
#include "SnapmakerU1Scripts.inc"
std::string normalized(std::string code)
{
    for (const auto& escape : {std::pair<std::string, std::string>{"\\n", "\n"}, {"\\r", "\r"}, {"\\t", "\t"}, {"\\\"", "\""}})
        for (size_t pos = 0; (pos = code.find(escape.first, pos)) != std::string::npos; pos += escape.second.size())
            code.replace(pos, escape.first.size(), escape.second);
    std::istringstream stream(code);
    std::string        result;
    for (std::string line; std::getline(stream, line);) {
        const auto first = std::find_if_not(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); });
        const auto last  = std::find_if_not(line.rbegin(), line.rend(), [](unsigned char c) { return std::isspace(c); }).base();
        if (first < last && *first != ';') {
            result.append(first, last);
            result += '\n';
        }
    }
    return result;
}
} // namespace

bool snapmaker_u1(const DynamicPrintConfig& config)
{
    const auto* model = config.option<ConfigOptionString>("printer_model");
    return model && model->value == "Snapmaker U1";
}

std::string validate_u1_scripts(const DynamicPrintConfig& config)
{
    for (const char* key : {"machine_start_gcode", "machine_end_gcode", "change_filament_gcode", "before_layer_change_gcode",
                            "layer_change_gcode", "time_lapse_gcode", "change_extrusion_role_gcode", "printing_by_object_gcode"}) {
        const auto*       option = config.option<ConfigOptionString>(key);
        const std::string value  = option ? normalized(option->value) : "";
        if (!value.empty() && std::none_of(std::begin(U1_STOCK_SCRIPTS), std::end(U1_STOCK_SCRIPTS),
                                           [&](const auto& entry) { return key == std::string(entry.first) && value == entry.second; }))
            return std::string("u1_custom_script_requires_adapter:") + key;
    }
    for (const char* key : {"filament_start_gcode", "filament_end_gcode"}) {
        const auto* option = config.option<ConfigOptionStrings>(key);
        if (option &&
            std::any_of(option->values.begin(), option->values.end(), [](const std::string& code) { return !normalized(code).empty(); }))
            return std::string("u1_custom_script_requires_adapter:") + key;
    }
    return {};
}

std::string u1_return_code(const Vec3d& position, double z_speed_mm_s)
{
    // Tn leaves XY at the dock and applies the calibrated tool offset. Force Z
    // there first, then both XY axes: writer position suppression is invalid here.
    GCodeG1Formatter z, xy;
    z.emit_z(position.z());
    z.emit_f(z_speed_mm_s * 60.);
    xy.emit_xy(position.head<2>());
    xy.emit_f(12000.);
    return "G90\n" + z.string() + xy.string();
}

std::string u1_start_code(const FinalPrintPlan& plan, PhysicalMaterialId initial)
{
    // Firmware contract: Snapmaker/u1-klipper 93cb12fa (extruder.py, fluidd.cfg).
    // All feeding, calibration and bin priming happen before any bed deposition.
    // The stock bed prime line is replaced by the validated kernel prime tower.
    const auto&        p     = plan.profile;
    const unsigned     first = initial.value - 1;
    const int          warm  = int(std::lround(p.nozzle_temperature[first]));
    std::ostringstream code;
    code.imbue(std::locale::classic());
    code << ";MB_MACHINE Snapmaker U1\n;MB_U1_START_BEGIN\nPRINT_START\nDEFECT_DETECTION_START\n"
         << "SET_PRINT_STATS_INFO TOTAL_LAYER=" << plan.cycles.size() << " CURRENT_LAYER=0\nTIMELAPSE_START\nM140 S"
         << p.initial_bed_temperature << "\nM104 T" << first << " S140\nM204 S10000\n";
    if (p.chamber_cooling_mode == 0)
        code << "SET_PURIFIER_MODE MODE=3 DESIRE_TEMP=45 FAN_SPEED=0.6 DELAY_OFF=600\n";
    else if (p.chamber_cooling_mode == 2)
        code << "SET_PURIFIER_MODE MODE=1 DESIRE_TEMP=42 ALARM_TEMP=45 FAN_SPEED=0.6 DELAY_OFF=0\n";
    else
        code << "SET_PURIFIER_MODE MODE=3 DESIRE_TEMP=0 FAN_SPEED=0.6 DELAY_OFF=600\n";
    code << "G28 X Y\nDEFECT_DETECT_NOODLE_FIRST\nT" << first << "\nG90\nDEFECT_DETECTION_DETECT_BED\nSM_PRINT_CHECK_SWITCH_EXTRUDER\n";
    // EXTRUDER addresses physical heads, whereas Tn/INDEX follow the print's
    // firmware map. A two-material job may be mapped to physical heads 2/3.
    // Firmware guards these service commands using its physical used-head set.
    for (size_t i = 0; i < 4; ++i)
        code << "SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=" << i << " TEMP=140\nSM_PRINT_AUTO_FEED EXTRUDER=" << i
             << "\nSM_PRINT_FLOW_CALIBRATE EXTRUDER=" << i << "\n";
    for (size_t i = 0; i < plan.machine.material_count; ++i) {
        if (plan.object_volume_mm3[i] + plan.purge_volume_mm3[i] <= 0.)
            continue;
        code << "T" << i << "\nSM_PRINT_PREEXTRUDE_FILAMENT INDEX=" << i << " FORCE=1 TEMP=" << p.initial_nozzle_temperature[i]
             << " RETRACT_LENGTH=" << p.retract_mm[i] << " SPEED="
             << std::min(360., plan.machine.max_volume_mm3_s * 240. /
                                   (PI * plan.machine.filament_diameter_mm[i] * plan.machine.filament_diameter_mm[i]))
             << "\n";
    }
    for (size_t i = 0; i < 4; ++i)
        code << "M104 S0 T" << i << " A0\n";
    code << "T" << first << "\nM106 S255\nM106 P2 S0\nMOVE_TO_DISCARD_FILAMENT_POSITION\nM109 T" << first << " S" << warm - 90
         << "\nROUGHLY_CLEAN_NOZZLE_WITH_DISCARD\nMOVE_TO_XY_IDLE_POSITION_EXTRUDER\nG28 Z I140 J140\nDETECT_BED_PLATE\n"
         << "G90\nG0 Z5 F300\nMOVE_TO_DISCARD_FILAMENT_POSITION\nM109 S" << warm - 50
         << "\nROUGHLY_CLEAN_NOZZLE\nMOVE_TO_XY_IDLE_POSITION_EXTRUDER\nFINELY_CLEAN_NOZZLE_STAGE_1\nM104 S" << warm - 90
         << "\nG0 Z5 F300\nMOVE_TO_DISCARD_FILAMENT_POSITION\nROUGHLY_CLEAN_NOZZLE\nMOVE_TO_XY_IDLE_POSITION_EXTRUDER\n"
         << "FINELY_CLEAN_NOZZLE_STAGE_2\nM106 S255\nM109 S" << warm - 90 << "\nM190 S" << p.initial_bed_temperature
         << "\nM107 P2\nG90\nG0 Z5 F300\nWAIT_CHAMBER_TEMP TIMEOUT=180\nG28 Z";
    if (p.bed_probe_offset_mm != 0.)
        code << " Z_OFFSET=" << p.bed_probe_offset_mm;
    code << "\nBED_MESH_CALIBRATE PROBE_COUNT=11,11";
    if (p.bed_probe_offset_mm != 0.)
        code << " Z_OFFSET=" << p.bed_probe_offset_mm;
    code << "\nM400\n"
         << u1_return_code(plan.schedule->initial_position, plan.machine.max_z_mm_s) << (p.relative_extrusion ? "M83\n" : "M82\n")
         << "G92 E0\nM106 S0\n;MB_U1_START_END\n";
    return code.str();
}
} // namespace Slic3r::MaterialBand
