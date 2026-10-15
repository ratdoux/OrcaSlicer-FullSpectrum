#include <catch2/catch_test_macros.hpp>
#include "libslic3r/GCode/SeamAlignmentSearch.hpp"
#include <cstdlib>
#include <optional>
#include <string>
using namespace Slic3r;
namespace {
struct CacheMode
{
    std::optional<std::string> previous;
    CacheMode()
    {
        if (const char* value = std::getenv("ORCA_SEAM_ALIGNMENT"))
            previous = value;
        set("cache");
    }
    ~CacheMode() { set(previous ? previous->c_str() : nullptr); }
    static void set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("ORCA_SEAM_ALIGNMENT", value ? value : "");
#else
        if (value)
            setenv("ORCA_SEAM_ALIGNMENT", value, 1);
        else
            unsetenv("ORCA_SEAM_ALIGNMENT");
#endif
    }
};
} // namespace
TEST_CASE("Borrowed seam neighborhoods preserve order across hits and eviction", "[CPUSlicing][SeamCache]")
{
    CacheMode                                    mode;
    std::vector<PrintObjectSeamData::LayerSeams> layers(2);
    auto&                                        layer = layers.front();
    layer.perimeters.emplace_back();
    for (int i = 0; i < 32; ++i)
        layer.points.emplace_back(Vec3f(float(i % 8), float(i / 8), 0), layer.perimeters.front(), 0,
                                  SeamPlacerImpl::EnforcedBlockedSeamPoint::Neutral);
    layer.points_tree =
        std::make_unique<PrintObjectSeamData::SeamCandidatesTree>(SeamPlacerImpl::SeamCandidateCoordinateFunctor{layer.points},
                                                                  layer.points.size());
    const Vec3f query(2, 2, 0);
    const auto  expected = find_nearby_points(*layer.points_tree, query, 2.5f);
    REQUIRE(SeamAlignmentSearch::active() == nullptr);
    {
        SeamAlignmentSearch search(layers);
        CHECK(search.neighbors(0, query, 2.5f) == expected);
        const auto* data                             = search.neighbors(0, query, 2.5f).data();
        layer.perimeters.front().finalized           = true;
        layer.perimeters.front().final_seam_position = Vec3f(99, 99, 99);
        CHECK(search.neighbors(0, query, 2.5f) == expected);
        CHECK(search.neighbors(0, query, 2.5f).data() == data);
        CHECK(search.neighbors(1, query, 2.5f).empty());
        CHECK(search.neighbors(0, query, 0) == find_nearby_points(*layer.points_tree, query, 0.f));
        for (size_t i = 0; i < 131074; ++i)
            REQUIRE(search.neighbors(0, Vec3f(float(i + 100), 0, 0), 0.25f).empty());
        CHECK(search.neighbors(0, query, 2.5f) == expected);
        {
            SeamAlignmentSearch nested(layers);
            CHECK(SeamAlignmentSearch::active() == &nested);
        }
        CHECK(SeamAlignmentSearch::active() == &search);
    }
    CHECK(SeamAlignmentSearch::active() == nullptr);
}
