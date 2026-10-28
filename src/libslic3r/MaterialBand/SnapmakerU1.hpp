#ifndef slic3r_MaterialBand_SnapmakerU1_hpp_
#define slic3r_MaterialBand_SnapmakerU1_hpp_

#include "FinalPrintPlan.hpp"
#include <string>

namespace Slic3r {
class DynamicPrintConfig;
namespace MaterialBand {
// Stock U1 firmware owns homing, probing and docking. Object geometry, prime
// structure, tool order and every return from the dock remain kernel-owned.
bool        snapmaker_u1(const DynamicPrintConfig& config);
std::string validate_u1_scripts(const DynamicPrintConfig& config);
std::string u1_start_code(const FinalPrintPlan& plan, PhysicalMaterialId initial);
std::string u1_return_code(const Vec3d& position, double z_speed_mm_s);
} // namespace MaterialBand
} // namespace Slic3r
#endif
