#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/PreviewMemory.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace Slic3r;

namespace {

void initialize_processor(GCodeProcessor& processor)
{
    processor.reset();
    processor.apply_config(PrintConfig());
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
