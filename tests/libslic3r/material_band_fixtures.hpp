#ifndef slic3r_test_material_band_fixtures_hpp_
#define slic3r_test_material_band_fixtures_hpp_

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <nlohmann/json.hpp>

namespace MaterialBandFixtures {

struct Fixture
{
    nlohmann::json             manifest;
    Slic3r::Model              model;
    Slic3r::DynamicPrintConfig config;
};

nlohmann::json corpus();
void           enable_material_bands(Fixture& fixture);
Fixture        make_fixture(const std::string& id);
Fixture        make_feature_off_fixture(const std::string& id);
Fixture        make_axial_fixture(const std::string& shape, unsigned resolution = 32, double height = .48);
// Independent analytic target, NOT a kernel implementation or solver.
double target_fraction(const nlohmann::json& manifest, double x_mm);
double permitted_fraction(const nlohmann::json& manifest, double x_mm);

} // namespace MaterialBandFixtures

#endif
