#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/PreviewMemory.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <chrono>
#include <fstream>
#include <iostream>

using namespace Slic3r;

namespace {

void initialize_processor(GCodeProcessor& processor)
{
    processor.reset();
    processor.apply_config(PrintConfig(), .2);
    processor.initialize("preview-memory-test.gcode");
    processor.process_buffer("G21\nG90\nM83\nG1 X10 Y0 Z0.2 F600\n;TYPE:External perimeter\n;WIDTH:0.4\n;HEIGHT:0.2\n");
}

void check_non_arc_moves(const GCodeProcessorResult& result)
{
    for (const auto& move : result.moves) {
        if (!move.is_arc_move()) {
            INFO("G-code line " << move.gcode_id);
            REQUIRE(move.interpolation_points.empty());
            REQUIRE(move.interpolation_points.capacity() == 0);
        }
    }
}

} // namespace

TEST_CASE("Non-planar preview groups moves into nominal height bands", "[GCodeProcessor][PreviewLayers]")
{
    double nominal = .08;
    SECTION("0.08 mm layers") {}
    SECTION("0.12 mm layers") { nominal = .12; }
    PrintConfig config;
    config.initial_layer_print_height.value = .2;
    GCodeProcessor processor;
    processor.reset();
    processor.apply_config(config, nominal);
    processor.initialize("nominal-preview.gcode");
    processor.process_buffer("G21\nG90\nM83\nG1 X0 Y0 Z0.2 F600\n;TYPE:External perimeter\n;WIDTH:0.4\n;HEIGHT:0.08\n"
                             "G1 X1 E0.02\nG1 X2 Z0.27 E0.02\nG1 X3 Z0.34 E0.02\nG1 X4 Z0.26 E0.02\n"
                             "G1 X5 Z0.32 E0.02\nG0 Z5\nG0 X6\nG0 Z0.28\nG1 X7 E0.02\n");
    processor.finalize(false);
    const auto& result = processor.get_result();
    const auto& grid   = result.preview_layer_grid;
    REQUIRE(grid.enabled);
    CHECK(grid.height == Approx(nominal));
    CHECK(grid.first_height == Approx(.2));
    CHECK(grid.index_at(.2) == 0);
    CHECK(grid.index_at(.27) == 1);
    CHECK(grid.index_at(.34) == 2);
    CHECK(grid.index_at(.26) == 1);                    // Returning to a band does not create another layer.
    CHECK(grid.index_at(.2 + nominal + .000001) == 1); // Float/decimal roundoff.
    CHECK(grid.index_at(.2 + nominal + .001) == 2);
    CHECK(grid.top_z(2) == Approx(.2 + 2 * nominal));
    std::vector<int> bands;
    size_t           selected = 0;
    for (const auto& move : result.moves)
        if (move.type == EMoveType::Extrude) {
            bands.push_back(grid.index_at(move.position.z()));
            if (grid.contains(move.position.z(), grid.top_z(1), grid.top_z(1)))
                ++selected;
        }
    CHECK(selected == (nominal == .08 ? 3 : 4));
    sort_remove_duplicates(bands);
    CHECK(bands == std::vector<int>{0, 1, 2}); // Travel Z hops add no layers.
    GCodeProcessorResult copied;
    copied = result;
    CHECK(copied.preview_layer_grid.enabled);
    CHECK(copied.preview_layer_grid.index_at(.34) == 2);
    copied.reset();
    CHECK_FALSE(copied.preview_layer_grid.enabled);
}

TEST_CASE("Planar preview retains its original layer detection", "[GCodeProcessor][PreviewLayers]")
{
    GCodeProcessor processor;
    initialize_processor(processor);
    processor.process_buffer("G1 X20 E1\nG0 Z0.4\nG1 X30 E1\nG1 X40 E1\n");
    processor.finalize(false);
    CHECK_FALSE(processor.get_result().preview_layer_grid.enabled);
}

TEST_CASE("Non-planar preview bands use model Z after the printer offset", "[GCodeProcessor][PreviewLayers]")
{
    PrintConfig config;
    config.initial_layer_print_height.value = .2;
    config.z_offset.value                   = .5;
    GCodeProcessor processor;
    processor.reset();
    processor.apply_config(config, .08);
    processor.initialize("offset-preview.gcode");
    processor.process_buffer("G21\nG90\nM83\nG1 X0 Y0 Z0.7 F600\n;TYPE:External perimeter\n;WIDTH:0.4\n;HEIGHT:0.08\n"
                             "G1 X1 E0.02\nG1 X2 Z0.77 E0.02\nG1 X3 Z0.84 E0.02\n");
    processor.finalize(false);
    const auto& result = processor.get_result();
    REQUIRE(result.preview_layer_grid.enabled);
    std::vector<int> bands;
    for (const auto& move : result.moves)
        if (move.type == EMoveType::Extrude)
            bands.push_back(result.preview_layer_grid.index_at(move.position.z()));
    CHECK(bands == std::vector<int>{0, 1, 2});
}

TEST_CASE("Preview preserves explicit scarf and vase layers", "[GCodeProcessor][PreviewLayers]")
{
    PrintConfig config;
    config.initial_layer_print_height.value = .2;
    bool tagged = true;
    SECTION("Vase") { config.spiral_mode.value = true; }
    SECTION("Scarf") { config.has_scarf_joint_seam.value = true; }
    SECTION("Ordinary non-planar config") { tagged = false; }
    GCodeProcessor processor;
    processor.reset();
    processor.apply_config(config, .08);
    processor.initialize("tagged-preview.gcode");
    processor.process_buffer("G21\nG90\nM83\nG1 X0 Y0 Z0.2 F600\n;TYPE:External perimeter\n;WIDTH:0.4\n;HEIGHT:0.08\n"
                             "G1 X1 E0.02\nG1 X2 Z0.24 E0.02\nG1 X3 Z0.28 E0.02\n");
    processor.finalize(false);
    CHECK(processor.get_result().preview_layer_grid.enabled == !tagged);
    CHECK(processor.get_result().preview_layer_grid.height == Approx(.08));
}

TEST_CASE("Replay real velocity limit commands without geometry work", "[.][VelocityLimitReplay]")
{
    const char* path = std::getenv("ORCA_PARSER_REPLAY");
    REQUIRE(path);
    std::ifstream input(path);
    REQUIRE(input.good());
    std::string line, commands;
    size_t      count = 0;
    while (std::getline(input, line))
        if (line.rfind("SET_VELOCITY_LIMIT ", 0) == 0) {
            commands += line;
            commands += '\n';
            ++count;
        }
    REQUIRE(count > 0);
    PrintConfig config;
    config.gcode_flavor.value = gcfKlipper;
    GCodeProcessor processor;
    processor.reset();
    processor.apply_config(config);
    processor.initialize("velocity-replay.gcode");
    const auto started = std::chrono::steady_clock::now();
    processor.process_buffer(commands);
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    processor.process_buffer("G21\nG90\nM83\nG1 X0 Y0 Z0.2 F12000\nG1 X100 Y0 E2\nG1 X100 Y100 E2\n");
    processor.finalize(false);
    std::cout << "Velocity replay commands=" << count << " bytes=" << commands.size() << " elapsed_ms=" << elapsed
              << " estimated_motion_seconds=" << processor.get_time(PrintEstimatedStatistics::ETimeMode::Normal) << '\n';
}

TEST_CASE("Klipper velocity limits retain command matching and estimated motion times", "[GCodeProcessor][VelocityLimits]")
{
    auto estimate = [](const std::string& limits) {
        PrintConfig config;
        config.gcode_flavor.value = gcfKlipper;
        GCodeProcessor processor;
        processor.reset();
        processor.apply_config(config);
        processor.initialize("velocity-limits-test.gcode");
        processor.process_buffer("G21\nG90\nM83\nM204 S1000\nM203 X300 Y300\nM205 X10 Y10\n");
        processor.process_buffer(limits);
        processor.process_buffer("G1 X0 Y0 Z0.2 F12000\n;TYPE:External perimeter\n;WIDTH:0.4\n;HEIGHT:0.2\n"
                                 "G1 X100 Y0 E2\nG1 X100 Y100 E2\nG1 X0 Y100 E2\nG1 X0 Y0 E2\n"
                                 "G0 X50 Y50\nG0 X100 Y0\nG0 X0 Y0\n");
        processor.finalize(false);
        return processor.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    };
    const float initial = estimate("");
    REQUIRE(std::isfinite(initial));
    REQUIRE(initial > 0);
    REQUIRE(estimate("M204 S100\n") > initial);
    REQUIRE(estimate("M203 X30 Y30\n") > initial);
    REQUIRE(estimate("M205 X1 Y1\n") != initial);

    const std::vector<std::pair<std::string, std::string>> cases{
        {"ACCEL=100", "M204 S100\n"},
        {"VELOCITY=30", "M203 X30 Y30\n"},
        {"SQUARE_CORNER_VELOCITY=1", "M205 X1 Y1\n"},
        {"ACCEL_TO_DECEL=100", ""},
        {"ACCEL=", "M204 S0\n"},
        {"ACCEL=-100", "M204 S0\n"},
        {"ACCEL=1..2", "M204 S1\n"},
        {"ACCEL=2e3", "M204 S2\n"},
        {"ACCEL=100 ACCEL=200", "M204 S100\n"},
        {"accel=100", ""},
        {"XACCEL=100 ACCEL_TO_DECEL=200 ACCEL = 300", "M204 S300\n"},
        {"ACCEL=...5", "M204 S0\n"},
        {"ACCEL=.5.6", "M204 S.5\n"},
        {"ACCEL=+100", "M204 S0\n"},
        {"ACCEL\t=\t100.25", "M204 S100.25\n"},
        {"ACCEL=1e999", "M204 S1\n"},
        {"VELOCITY = 30.5\tACCEL = 100.5 SQUARE_CORNER_VELOCITY = .5", "M203 X30.5 Y30.5\nM204 S100.5\nM205 X.5 Y.5\n"},
    };
    for (const auto& [parameters, reference] : cases) {
        INFO(parameters);
        const std::string command = "SET_VELOCITY_LIMIT " + parameters + "\n";
        CHECK(estimate(command) == estimate(reference));
        CHECK(estimate(command + command) == estimate(reference));
    }
}

TEST_CASE("Arc interpolation belongs only to arc moves across streaming buffers", "[GCodeProcessor][PreviewMemory]")
{
    GCodeProcessor processor;
    initialize_processor(processor);
    processor.set_xy_offset(3.0, 4.0);
    processor.process_buffer("G3 X0 Y10 I-10 J0 E1 F600\n");
    REQUIRE(processor.get_result().moves.back().is_arc_move());
    const auto first_arc = processor.get_result().moves.back();
    REQUIRE_FALSE(first_arc.interpolation_points.empty());
    REQUIRE(first_arc.arc_center_position.isApprox(Vec3f(3.0f, 4.0f, 0.2f)));
    for (const Vec3f& point : first_arc.interpolation_points)
        REQUIRE(std::abs((point - first_arc.arc_center_position).head<2>().norm() - 10.0f) < 0.0001f);

    // Include extrusion, travel, retraction and unretraction after the arc.
    processor.process_buffer("G1 X1 Y11 E0.1\nG0 X0 Y10\nG1 E-1\nG1 E1\n");
    processor.process_buffer("G2 X10 Y0 I0 J-10 E1\nG1 X11 Y0 E0.1\n");
    processor.finalize(false);

    const auto& result = processor.get_result();
    REQUIRE(std::count_if(result.moves.begin(), result.moves.end(), [](const auto& move) { return move.is_arc_move(); }) == 2);
    check_non_arc_moves(result);
    const auto arc = std::find_if(result.moves.begin(), result.moves.end(), [](const auto& move) { return move.is_arc_move(); });
    REQUIRE(arc->interpolation_points.size() == first_arc.interpolation_points.size());
    for (size_t i = 0; i < first_arc.interpolation_points.size(); ++i)
        REQUIRE(arc->interpolation_points[i].isApprox(first_arc.interpolation_points[i]));
    REQUIRE(result.moves.back().position.isApprox(Vec3f(14.0f, 4.0f, 0.2f)));
}

TEST_CASE("A reused processor does not carry arc data into the next file", "[GCodeProcessor][PreviewMemory]")
{
    GCodeProcessor processor;
    initialize_processor(processor);
    processor.process_buffer("G3 X0 Y10 I-10 J0 E1\n");
    REQUIRE_FALSE(processor.get_result().moves.back().interpolation_points.empty());

    initialize_processor(processor);
    processor.process_buffer("G1 X11 Y0 E0.1\nG1 X12 Y1 E0.1\n");
    processor.finalize(false);
    check_non_arc_moves(processor.get_result());
}

TEST_CASE("Completed and copied G-code results release oversized allocations", "[GCodeProcessor][PreviewMemory]")
{
    GCodeProcessor processor;
    initialize_processor(processor);
    processor.process_buffer("G3 X0 Y10 I-10 J0 E1\nG1 X1 Y11 E0.1\n");
    processor.result().moves.reserve(100000);
    processor.result().lines_ends.reserve(400000);
    processor.result().lines_ends = {12, 34, 56};
    const auto expected_moves     = processor.get_result().moves;

    processor.finalize(false);
    REQUIRE(processor.get_result().moves.capacity() < 100000);
    REQUIRE(processor.get_result().lines_ends.capacity() < 400000);
    REQUIRE(processor.get_result().moves.size() == expected_moves.size());

    GCodeProcessorResult preview{};
    preview.moves.reserve(100000);
    preview.lines_ends.reserve(400000);
    // Export and plate switching use result assignment; the destination may have
    // held a much larger slice even when the incoming result is already compact.
    preview = processor.get_result();
    REQUIRE(preview.moves.capacity() < 100000);
    REQUIRE(preview.lines_ends.capacity() < 400000);
    REQUIRE(preview.lines_ends == std::vector<size_t>{12, 34, 56});
    REQUIRE(preview.moves.size() == expected_moves.size());
    for (size_t i = 0; i < expected_moves.size(); ++i) {
        REQUIRE(preview.moves[i].gcode_id == expected_moves[i].gcode_id);
        REQUIRE(preview.moves[i].position.isApprox(expected_moves[i].position));
        REQUIRE(preview.moves[i].interpolation_points.size() == expected_moves[i].interpolation_points.size());
        for (size_t j = 0; j < expected_moves[i].interpolation_points.size(); ++j)
            REQUIRE(preview.moves[i].interpolation_points[j].isApprox(expected_moves[i].interpolation_points[j]));
    }

    preview.moves.reserve(100000);
    preview.lines_ends.reserve(400000);
    preview.reset();
    REQUIRE(preview.moves.empty());
    REQUIRE(preview.lines_ends.empty());
    REQUIRE(preview.moves.capacity() == 0);
    REQUIRE(preview.lines_ends.capacity() == 0);
}

TEST_CASE("Preview memory compaction preserves small buffers and useful growth headroom", "[PreviewMemory]")
{
    SECTION("Small reusable buffers keep their allocation")
    {
        std::vector<unsigned int> buffer(16, 7);
        buffer.reserve(1024);
        const auto* data = buffer.data();
        GCodePreview::shrink_excess_capacity(buffer);
        REQUIRE(buffer.data() == data);
        REQUIRE(buffer.size() == 16);
        REQUIRE(buffer.back() == 7);
    }

    SECTION("Large buffers with modest spare capacity keep their allocation")
    {
        std::vector<unsigned int> buffer;
        buffer.reserve(4000000);
        buffer.resize(buffer.capacity() * 9 / 10, 7);
        const auto* data = buffer.data();
        GCodePreview::shrink_excess_capacity(buffer);
        REQUIRE(buffer.data() == data);
        REQUIRE(buffer.back() == 7);
    }

    SECTION("Large surplus storage is released without copying owned payloads")
    {
        std::vector<std::unique_ptr<int>> buffer;
        buffer.reserve(400000);
        buffer.push_back(std::make_unique<int>(42));
        const auto* value = buffer.front().get();
        GCodePreview::shrink_excess_capacity(buffer);
        REQUIRE(buffer.capacity() < 400000);
        REQUIRE(buffer.front().get() == value);
        REQUIRE(*buffer.front() == 42);
    }
}
