#ifndef slic3r_LayerGeometryBatch_hpp_
#define slic3r_LayerGeometryBatch_hpp_

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace Slic3r {

struct LayerGeometryBatch
{
    size_t begin, end, neighbor_begin, neighbor_end, input_points;
};

// Always admit one layer, including an oversized one, so every iteration makes
// progress. The point budget bounds input admission, not Boolean output size.
template<class CountPoints>
LayerGeometryBatch next_layer_geometry_batch(
    size_t begin, size_t layer_count, size_t layer_limit, size_t point_limit, CountPoints count_points)
{
    assert(begin < layer_count && layer_limit > 0);
    LayerGeometryBatch batch{begin, begin, begin > 0 ? begin - 1 : 0, begin, 0};
    while (batch.end < layer_count && batch.end - begin < layer_limit) {
        const size_t points = count_points(batch.end);
        if (batch.end != begin && points > point_limit - std::min(batch.input_points, point_limit))
            break;
        batch.input_points += points;
        ++batch.end;
    }
    batch.neighbor_end = batch.end < layer_count ? batch.end + 1 : layer_count;
    return batch;
}

} // namespace Slic3r
#endif
