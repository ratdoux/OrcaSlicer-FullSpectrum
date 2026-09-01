#include <catch2/catch_test_macros.hpp>

#include "libslic3r/SSWCPProtocol.hpp"

#include <nlohmann/json.hpp>

#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace Slic3r;

TEST_CASE("SSWCP file flow mappings are aligned and normalized", "[SSWCPProtocol]")
{
    CHECK(SSWCPProtocol::build_filament_volume_types(nullptr, 3) ==
          std::vector<std::string>{"standard", "standard", "standard"});

    const std::vector<int> values{fvtStandard, fvtHighFlow};
    CHECK(SSWCPProtocol::build_filament_volume_types(&values, 4) ==
          std::vector<std::string>{"standard", "high_flow", "standard", "standard"});

    const std::vector<int> unknown{fvtHighFlow, 99};
    CHECK(SSWCPProtocol::build_filament_volume_types(&unknown, 2) ==
          std::vector<std::string>{"high_flow", "standard"});
}

TEST_CASE("SSWCP machine update flow types are optional but validated", "[SSWCPProtocol]")
{
    std::vector<std::string> flows;
    CHECK(SSWCPProtocol::parse_optional_flow_types(json::object(), "nozzle_volume_types", 2, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Missing);

    const json valid = {{"nozzle_volume_types", {"standard", "high_flow"}}};
    CHECK(SSWCPProtocol::parse_optional_flow_types(valid, "nozzle_volume_types", 2, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Valid);
    CHECK(flows == std::vector<std::string>{"standard", "high_flow"});

    for (const json &invalid : std::vector<json>{
             {{"nozzle_volume_types", "standard"}},
             {{"nozzle_volume_types", {"standard"}}},
             {{"nozzle_volume_types", {"standard", 1}}},
             {{"nozzle_volume_types", {"standard", "tpu_high_flow"}}}}) {
        CHECK(SSWCPProtocol::parse_optional_flow_types(invalid, "nozzle_volume_types", 2, flows) ==
              SSWCPProtocol::OptionalFlowTypesStatus::Invalid);
        CHECK(flows.empty());
    }
}

TEST_CASE("SSWCP system info preserves supported scalar and array nozzle data", "[SSWCPProtocol]")
{
    const json response = {{"data", {{"system_info", {{"product_info", {
        {"machine_type", "Snapmaker U1"},
        {"device_name", "Workshop"},
        {"nozzle_diameter", {0.2, 0.4, 0.6, 0.8}},
        {"nozzle_volume_type", {"standard", "high_flow", "standard", "high_flow"}}
    }}}}}}};

    MachineInfo info;
    REQUIRE(SSWCPProtocol::parse_machine_info_response(response, info));
    CHECK(info.model == "Snapmaker U1");
    CHECK(info.device_name == "Workshop");
    CHECK(info.nozzle_diameters == std::vector<std::string>{"0.2", "0.4", "0.6", "0.8"});
    CHECK(info.nozzle_volume_types ==
          std::vector<std::string>{"standard", "high_flow", "standard", "high_flow"});

    const json scalar = {{"system_info", {{"product_info", {
        {"nozzle_diameter", 0.8}, {"nozzle_volume_type", "high_flow"}
    }}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(scalar, info));
    CHECK(info.nozzle_diameters == std::vector<std::string>{"0.8"});
    CHECK(info.nozzle_volume_types == std::vector<std::string>{"high_flow"});

    for (const auto &[diameter, expected] : std::vector<std::pair<double, std::string>>{
             {0.2, "0.2"}, {0.4, "0.4"}, {0.6, "0.6"}, {0.8, "0.8"}}) {
        const json single = {{"system_info", {{"product_info", {{"nozzle_diameter", diameter}}}}}};
        REQUIRE(SSWCPProtocol::parse_machine_info_response(single, info));
        CHECK(info.nozzle_diameters == std::vector<std::string>{expected});
    }
}

TEST_CASE("SSWCP system info tolerates unavailable flow data", "[SSWCPProtocol]")
{
    MachineInfo info;
    const json missing = {{"system_info", {{"product_info", {{"machine_type", "Snapmaker U1"}}}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(missing, info));
    CHECK(info.nozzle_volume_types.empty());

    const json invalid = {{"system_info", {{"product_info", {
        {"machine_type", "Snapmaker U1"}, {"nozzle_volume_type", {"standard", "tpu_high_flow"}}
    }}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(invalid, info));
    CHECK(info.nozzle_volume_types.empty());
}

TEST_CASE("Complete cached slots override direct nozzle data atomically", "[SSWCPProtocol]")
{
    std::vector<std::string> diameters{"0.8"};
    std::vector<std::string> flows{"high_flow"};
    REQUIRE(SSWCPProtocol::select_complete_cached_nozzle_info(
        {{"0.4", "standard"}, {"0.4", "high_flow"}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.4", "0.4"});
    CHECK(flows == std::vector<std::string>{"standard", "high_flow"});

    diameters = {"0.8"};
    flows     = {"high_flow"};
    CHECK_FALSE(SSWCPProtocol::select_complete_cached_nozzle_info(
        {{"0.4", "standard"}, {"", "high_flow"}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.8"});
    CHECK(flows == std::vector<std::string>{"high_flow"});

    diameters = {"0.8"};
    flows     = {"high_flow"};
    REQUIRE(SSWCPProtocol::select_complete_cached_nozzle_info(
        {{"0.4", "standard"}, {"0.4", ""}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.4", "0.4"});
    CHECK(flows.empty());
}

TEST_CASE("tpu_high_flow is never emitted in any protocol output", "[SSWCPProtocol]")
{
    // File mapping: any non-fvtHighFlow value (including future enum additions) maps to standard.
    const std::vector<int> with_unknown{fvtHighFlow, 2, 99, fvtStandard};
    const auto             mapped = SSWCPProtocol::build_filament_volume_types(&with_unknown, 4);
    for (const std::string &v : mapped)
        CHECK(v != "tpu_high_flow");

    // Machine update: tpu_high_flow is explicitly rejected as Invalid.
    std::vector<std::string> flows;
    const json               bad_update = {{"nozzle_volume_types", {"tpu_high_flow", "standard"}}};
    CHECK(SSWCPProtocol::parse_optional_flow_types(bad_update, "nozzle_volume_types", 2, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Invalid);

    // System info: tpu_high_flow leaves flow unavailable, never stored.
    MachineInfo  info;
    const json   bad_system = {{"system_info", {{"product_info", {
        {"machine_type", "X"}, {"nozzle_volume_type", {"tpu_high_flow", "high_flow"}}
    }}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(bad_system, info));
    CHECK(info.nozzle_volume_types.empty());
    for (const std::string &v : info.nozzle_volume_types)
        CHECK(v != "tpu_high_flow");
}

TEST_CASE("Machine model normalization handles aliases and preserves empty", "[SSWCPProtocol]")
{
    // Empty must stay empty — never default to a machine type.
    CHECK(SSWCPProtocol::normalize_machine_model("") == "");

    // Known firmware aliases.
    CHECK(SSWCPProtocol::normalize_machine_model("lava") == "Snapmaker U1");
    CHECK(SSWCPProtocol::normalize_machine_model("Snapmaker test") == "Snapmaker U1");

    // Already-normalized values pass through unchanged (idempotent).
    CHECK(SSWCPProtocol::normalize_machine_model("Snapmaker U1") == "Snapmaker U1");
    const auto once = SSWCPProtocol::normalize_machine_model("Snapmaker Artisan");
    CHECK(SSWCPProtocol::normalize_machine_model(once) == once);

    // Unknown models pass through as-is.
    CHECK(SSWCPProtocol::normalize_machine_model("SomeFuturePrinter") == "SomeFuturePrinter");
}

TEST_CASE("parse_extruder_nozzle_info extracts real-time nozzle data", "[SSWCPProtocol]")
{
    json response = R"({
        "data": {
            "status": {
                "extruder": {
                    "nozzle_diameter": "0.4",
                    "nozzle_volume_type": "standard"
                },
                "extruder1": {
                    "nozzle_diameter": 0.6,
                    "nozzle_volume_type": "high_flow"
                }
            }
        }
    })"_json;

    std::vector<std::string> diameters, flows;
    CHECK(SSWCPProtocol::parse_extruder_nozzle_info(response, diameters, flows));
    REQUIRE(diameters.size() == 2);
    CHECK(diameters[0] == "0.4");
    CHECK(diameters[1] == "0.6");
    REQUIRE(flows.size() == 2);
    CHECK(flows[0] == "standard");
    CHECK(flows[1] == "high_flow");

    // Missing status -> false
    json empty = json::object();
    CHECK(!SSWCPProtocol::parse_extruder_nozzle_info(empty, diameters, flows));

    // Invalid diameter value -> skipped, returns false when none valid
    json bad = json::parse(R"({"data":{"status":{"extruder":{"nozzle_diameter":"0.99"}}}})");
    CHECK(!SSWCPProtocol::parse_extruder_nozzle_info(bad, diameters, flows));

    // Missing flow type defaults to standard
    json no_flow = json::parse(R"({"data":{"status":{"extruder":{"nozzle_diameter":"0.8"}}}})");
    CHECK(SSWCPProtocol::parse_extruder_nozzle_info(no_flow, diameters, flows));
    // Missing nozzle_volume_type means firmware doesn't report flow: leave flows empty
    CHECK(flows.empty());
}

TEST_CASE("parse_extruder_nozzle_info dynamically discovers and orders extruders", "[SSWCPProtocol]")
{
    // Keys in non-sequential order: extruder1 before extruder.
    // Also tests that non-extruder keys (gcode_move, heater_bed) are ignored.
    json response = json::parse(R"({
        "data": {
            "status": {
                "gcode_move": {},
                "extruder1": {"nozzle_diameter": "0.6", "nozzle_volume_type": "high_flow"},
                "extruder": {"nozzle_diameter": "0.4", "nozzle_volume_type": "standard"},
                "heater_bed": {},
                "extruder2": {"nozzle_diameter": "0.8"}
            }
        }
    })");

    std::vector<std::string> diameters, flows;
    CHECK(SSWCPProtocol::parse_extruder_nozzle_info(response, diameters, flows));
    REQUIRE(diameters.size() == 3);
    CHECK(diameters[0] == "0.4");
    CHECK(diameters[1] == "0.6");
    CHECK(diameters[2] == "0.8");
    // extruder2 is missing flow type -> all_flows_present is false -> flows cleared
    CHECK(flows.empty());
}

TEST_CASE("parse_extruder_nozzle_info ignores pathological extruder key suffixes", "[SSWCPProtocol]")
{
    // A key with an absurdly long numeric suffix must be skipped, not crash stoul.
    json response = json::parse(R"({
        "data": {
            "status": {
                "extruder99999999999999999": {"nozzle_diameter": "0.4", "nozzle_volume_type": "standard"}
            }
        }
    })");

    std::vector<std::string> diameters, flows;
    CHECK(!SSWCPProtocol::parse_extruder_nozzle_info(response, diameters, flows));
    CHECK(diameters.empty());

    // Sanity: a reasonable multi-digit suffix still works.
    json ok = json::parse(R"({
        "data": {
            "status": {
                "extruder10": {"nozzle_diameter": "0.8", "nozzle_volume_type": "high_flow"}
            }
        }
    })");
    CHECK(SSWCPProtocol::parse_extruder_nozzle_info(ok, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.8"});
}
