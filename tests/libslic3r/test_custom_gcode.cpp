#include <catch2/catch_test_macros.hpp>

#include "libslic3r/CustomGCode.hpp"

using namespace Slic3r;

TEST_CASE("Custom layer tool changes keep mixed virtual filament ids", "[CustomGCode]")
{
    CustomGCode::Info info;
    info.gcodes.emplace_back(CustomGCode::Item{1.25, CustomGCode::ToolChange, 5, "", ""});

    const auto tool_changes = CustomGCode::custom_tool_changes(info, 6);

    REQUIRE(tool_changes.size() == 1);
    CHECK_THAT(tool_changes.front().first, WithinRel(1.25, 0.001));
    CHECK(tool_changes.front().second == 5u);
}

TEST_CASE("Custom layer tool changes still clamp stale filament ids", "[CustomGCode]")
{
    CustomGCode::Info info;
    info.gcodes.emplace_back(CustomGCode::Item{2.0, CustomGCode::ToolChange, 7, "", ""});

    const auto tool_changes = CustomGCode::custom_tool_changes(info, 6);

    REQUIRE(tool_changes.size() == 1);
    CHECK(tool_changes.front().second == 1u);
}
