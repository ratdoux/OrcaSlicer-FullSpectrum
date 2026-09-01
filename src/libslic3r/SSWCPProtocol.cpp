#include "SSWCPProtocol.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <utility>

namespace Slic3r { namespace SSWCPProtocol {

namespace {

bool is_valid_flow_type(const std::string &value)
{
    // Only the standard and high-flow nozzle modes are recognized flow types.
    return value == FLOW_MODE_STANDARD || value == FLOW_MODE_HIGH_FLOW;
}

bool append_nozzle_diameter(const nlohmann::json &value, std::vector<std::string> &out)
{
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text == "0.2" || text == "0.4" || text == "0.6" || text == "0.8") {
            out.push_back(text);
            return true;
        }
        return false;
    }

    if (!value.is_number())
        return false;

    const double diameter = value.get<double>();
    for (const auto &[number, text] : std::vector<std::pair<double, const char *>>{
             {0.2, "0.2"}, {0.4, "0.4"}, {0.6, "0.6"}, {0.8, "0.8"}}) {
        if (std::abs(diameter - number) < 1e-6) {
            out.emplace_back(text);
            return true;
        }
    }
    return false;
}

bool parse_system_flow_types(const nlohmann::json &value, std::vector<std::string> &out)
{
    std::vector<std::string> parsed;
    const auto               append = [&parsed](const nlohmann::json &entry) {
        if (!entry.is_string()) return false;
        const std::string type = entry.get<std::string>();
        if (!is_valid_flow_type(type)) return false;
        parsed.push_back(type);
        return true;
    };

    if (value.is_array()) {
        for (const nlohmann::json &entry : value)
            if (!append(entry)) return false;
    } else if (!append(value)) {
        return false;
    }

    out = std::move(parsed);
    return true;
}

} // namespace

std::vector<std::string> build_filament_volume_types(const std::vector<int> *values, size_t count)
{
    std::vector<std::string> result(count, FLOW_MODE_STANDARD);
    if (values == nullptr) return result;
    for (size_t i = 0; i < std::min(count, values->size()); ++i)
        if ((*values)[i] == fvtHighFlow) result[i] = FLOW_MODE_HIGH_FLOW;
    return result;
}

OptionalFlowTypesStatus parse_optional_flow_types(const nlohmann::json &object, const char *key, size_t count,
                                                   std::vector<std::string> &out)
{
    out.clear();
    const auto it = object.find(key);
    if (it == object.end()) return OptionalFlowTypesStatus::Missing;
    if (!it->is_array() || it->size() < count) return OptionalFlowTypesStatus::Invalid;

    std::vector<std::string> parsed;
    parsed.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (!(*it)[i].is_string()) return OptionalFlowTypesStatus::Invalid;
        const std::string type = (*it)[i].get<std::string>();
        if (!is_valid_flow_type(type)) return OptionalFlowTypesStatus::Invalid;
        parsed.push_back(type);
    }
    out = std::move(parsed);
    return OptionalFlowTypesStatus::Valid;
}

bool parse_machine_info_response(const nlohmann::json &response, MachineInfo &out)
{
    const nlohmann::json *root = &response;
    if (const auto data = response.find("data"); data != response.end() && data->is_object())
        root = &*data;

    const auto system_info = root->find("system_info");
    if (system_info == root->end() || !system_info->is_object()) return false;
    const auto product_info = system_info->find("product_info");
    if (product_info == system_info->end() || !product_info->is_object()) return false;

    MachineInfo parsed;
    if (const auto model = product_info->find("machine_type"); model != product_info->end() && model->is_string())
        parsed.model = model->get<std::string>();
    if (const auto name = product_info->find("device_name"); name != product_info->end() && name->is_string())
        parsed.device_name = name->get<std::string>();

    if (const auto diameters = product_info->find("nozzle_diameter"); diameters != product_info->end()) {
        if (diameters->is_array()) {
            for (const nlohmann::json &entry : *diameters)
                append_nozzle_diameter(entry, parsed.nozzle_diameters);
        } else {
            append_nozzle_diameter(*diameters, parsed.nozzle_diameters);
        }
    }

    if (const auto flows = product_info->find("nozzle_volume_type"); flows != product_info->end() &&
        !parse_system_flow_types(*flows, parsed.nozzle_volume_types))
        parsed.nozzle_volume_types.clear();

    out = std::move(parsed);
    return true;
}

bool select_complete_cached_nozzle_info(const std::vector<std::pair<std::string, std::string>> &slots,
                                         std::vector<std::string> &diameters, std::vector<std::string> &flows)
{
    if (slots.empty()) return false;

    std::vector<std::string> cached_diameters;
    std::vector<std::string> cached_flows;
    cached_diameters.reserve(slots.size());
    cached_flows.reserve(slots.size());
    bool flows_complete = true;
    for (const auto &[diameter, flow] : slots) {
        if (diameter.empty()) return false;
        cached_diameters.push_back(diameter);
        if (!is_valid_flow_type(flow))
            flows_complete = false;
        else
            cached_flows.push_back(flow);
    }

    diameters = std::move(cached_diameters);
    if (flows_complete)
        flows = std::move(cached_flows);
    // else: leave the caller's flows untouched. An incomplete cache must not wipe
    // values we already have (e.g. freshly resolved from objects.query); only a
    // complete, valid cache overrides them.
    return true;
}

std::string normalize_machine_model(const std::string &model)
{
    // CRITICAL: empty string must stay empty. Never default to a machine type,
    // because callers use the result for whitelist gating and persistent storage.
    if (model.empty())
        return "";

    // Known firmware aliases that map to Snapmaker U1.
    if (model == "lava" || model == "Snapmaker test")
        return "Snapmaker U1";

    return model;
}

bool parse_extruder_nozzle_info(const nlohmann::json &response,
                                 std::vector<std::string> &diameters,
                                 std::vector<std::string> &volume_types)
{
    diameters.clear();
    volume_types.clear();

    // Navigate to data.status
    const nlohmann::json *root = &response;
    if (const auto data = response.find("data"); data != response.end() && data->is_object())
        root = &*data;
    const auto status = root->find("status");
    if (status == root->end() || !status->is_object())
        return false;

    // Dynamically discover extruder objects: "extruder", "extruder1", "extruder2", ...
    // Moonraker names the first toolhead "extruder" and subsequent ones with a numeric suffix.
    // Collect matching keys with their ordinal index for stable ordering.
    struct ExtruderEntry { size_t index; std::string key; };
    std::vector<ExtruderEntry> extruders;
    for (auto it = status->begin(); it != status->end(); ++it) {
        const std::string &key = it.key();
        size_t index;
        if (key == "extruder") {
            index = 0;
        } else if (key.size() > 8 && key.compare(0, 8, "extruder") == 0 &&
                   std::all_of(key.begin() + 8, key.end(), [](unsigned char c) { return std::isdigit(c); })) {
            // Cap suffix length to avoid stoul overflow on pathological firmware keys.
            if (key.size() - 8 > 4) continue;
            index = std::stoul(key.substr(8));
        } else {
            continue;
        }
        if (it->is_object())
            extruders.push_back({index, key});
    }
    // Sort by toolhead index: extruder(0), extruder1(1), extruder2(2), ...
    std::sort(extruders.begin(), extruders.end(),
              [](const ExtruderEntry &a, const ExtruderEntry &b) { return a.index < b.index; });

    bool all_flows_present = true;
    for (const auto &entry : extruders) {
        const auto extr = status->find(entry.key);
        const auto nd = extr->find("nozzle_diameter");
        if (nd == extr->end())
            continue;

        if (!append_nozzle_diameter(*nd, diameters))
            continue;

        const auto vt = extr->find("nozzle_volume_type");
        if (vt != extr->end() && vt->is_string()) {
            const std::string flow = vt->get<std::string>();
            volume_types.push_back(is_valid_flow_type(flow) ? flow : FLOW_MODE_STANDARD);
        } else {
            all_flows_present = false;
        }
    }

    if (!all_flows_present)
        volume_types.clear();

    return !diameters.empty();
}

}} // namespace Slic3r::SSWCPProtocol
