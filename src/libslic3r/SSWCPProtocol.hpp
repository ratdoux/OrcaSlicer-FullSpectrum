#ifndef slic3r_SSWCPProtocol_hpp_
#define slic3r_SSWCPProtocol_hpp_

#include "libslic3r/PrintConfig.hpp"
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

/// Machine identification result parsed from the firmware system_info response.
struct MachineInfo {
    std::string              model;
    std::string              device_name;
    std::vector<std::string> nozzle_diameters;
    std::vector<std::string> nozzle_volume_types;
};

/// Pure protocol-conversion helpers shared by SSWCP handlers and tests.
/// No wxWidgets dependency — safe to unit-test without a GUI event loop.
namespace SSWCPProtocol {

/// Result of parsing the optional nozzle_volume_types field in a machine update payload.
enum class OptionalFlowTypesStatus { Missing, Valid, Invalid };

/// Build a filament_volume_type string array aligned with the logical filament count.
/// Values other than fvtHighFlow are normalized to "standard".
/// If *values* is null, every entry is "standard".
std::vector<std::string> build_filament_volume_types(const std::vector<int> *values, size_t count);

/// Parse an optional flow-types array from a JSON object.
/// - Missing key → Missing (caller treats as "no flow data")
/// - Present but wrong type / short / invalid value → Invalid (caller rejects the payload)
/// - Present and valid → Valid, fills *out*
OptionalFlowTypesStatus parse_optional_flow_types(const nlohmann::json &object, const char *key, size_t count,
                                                   std::vector<std::string> &out);

/// Parse a firmware system_info response into a MachineInfo.
/// Accepts both scalar and array nozzle_diameter, and scalar/array nozzle_volume_type.
/// Unknown flow strings leave nozzle_volume_types empty rather than defaulting to standard.
/// Returns false only when system_info or product_info is absent.
bool parse_machine_info_response(const nlohmann::json &response, MachineInfo &out);

/// If every cached slot has a non-empty diameter, replace *diameters* with the cached
/// values and replace *flows* only when every cached flow is also valid.
/// Returns false (leaving outputs untouched) when any diameter is empty or slots is empty.
bool select_complete_cached_nozzle_info(const std::vector<std::pair<std::string, std::string>> &slots,
                                         std::vector<std::string> &diameters, std::vector<std::string> &flows);

/// Normalize a machine model identifier across all sources (system_info, SSDP, DeviceManager).
/// Applies known firmware aliases (e.g. "lava"/"Snapmaker test" -> "Snapmaker U1").
/// CRITICAL: empty input always returns empty — never defaults to a machine type.
/// Idempotent: normalize(normalize(x)) == normalize(x).
std::string normalize_machine_model(const std::string &model);

/// Resolution outcome distinguishing "no network response" from "got identity but missing fields".
enum class ResolveStatus {
    NoResponse,   ///< All network requests failed or timed out
    GotIdentity,  ///< model resolved (nozzle data may still be missing)
    Complete      ///< All fields (model + nozzle) filled
};

/// Result of resolve_machine_info.
struct ResolveResult {
    ResolveStatus status {ResolveStatus::NoResponse};
    MachineInfo   info;
};

/// Parse nozzle_diameter / nozzle_volume_type from printer.objects.query extruder objects.
/// Dynamically discovers extruder / extruder1 / ... / extruderN keys in the response status.
/// Returns true if at least one extruder with a valid nozzle_diameter was found.
bool parse_extruder_nozzle_info(const nlohmann::json &response,
                                 std::vector<std::string> &diameters,
                                 std::vector<std::string> &volume_types);

/// sw_FinishFilamentMapping params.event — selects the action to run after the
/// preprint webview closes. Extensible: add a new value here, then handle it in
/// SSWCP_MachineOption_Instance (a dedicated on_finish_* handler + a switch case).
///   0 / absent: close the webview dialog, no further action
///   1: open the custom filament flow regrouping dialog
enum class FinishFilamentMappingEvent {
    None              = 0,
    CustomFlowRegroup = 1,
};

} // namespace SSWCPProtocol
} // namespace Slic3r

#endif // slic3r_SSWCPProtocol_hpp_
