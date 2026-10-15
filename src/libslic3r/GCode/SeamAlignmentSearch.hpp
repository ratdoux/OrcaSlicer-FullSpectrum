#ifndef slic3r_SeamAlignmentSearch_hpp_
#define slic3r_SeamAlignmentSearch_hpp_
#include "SeamPlacer.hpp"
namespace Slic3r {
class SeamAlignmentSearch
{
public:
    explicit SeamAlignmentSearch(const std::vector<PrintObjectSeamData::LayerSeams>& layers);
    ~SeamAlignmentSearch();
    static SeamAlignmentSearch* active();
    // Borrowed until the next neighbors() call on this search or its destruction.
    const std::vector<size_t>& neighbors(size_t layer, const Vec3f& center, float radius);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    SeamAlignmentSearch*  m_previous = nullptr;
};
} // namespace Slic3r
#endif
