#ifndef slic3r_PolygonCompute_hpp_
#define slic3r_PolygonCompute_hpp_

#include "Polygon.hpp"
#include "clipper.hpp"
#include <chrono>

namespace Slic3r {

// Worker-local attribution/admission. The GPU backend serializes its queue;
// scopes do not leak from one TBB task to the next.
class PolygonComputeScope
{
public:
    enum class Stage { None, Walls, Shells, Paint };
    explicit PolygonComputeScope(Stage stage);
    ~PolygonComputeScope();
    static void shutdown(); // Before host-DLL teardown, after jobs have joined.
private:
    Stage m_previous;
};

class PolygonComputeQuery
{
public:
    PolygonComputeQuery(ClipperLib::ClipType operation, ClipperLib::PolyFillType fill);
    bool                                     active() const { return m_active; }
    template<class Subject, class Clip> void prepare(const Subject& subject, const Clip& clip)
    {
        if (!m_active)
            return;
        for (const Points& path : subject)
            m_subject.push_back(&path);
        for (const Points& path : clip)
            m_clip.push_back(&path);
        classify();
    }
    bool skip() const;
    void finish(bool empty, bool skipped);

private:
    void                                  classify();
    bool                                  m_active = false, m_validate = false, m_eligible = false, m_proven = false, m_gpu = false;
    PolygonComputeScope::Stage            m_stage;
    ClipperLib::ClipType                  m_operation;
    ClipperLib::PolyFillType              m_fill;
    size_t                                m_subject_edges = 0, m_clip_edges = 0;
    double                                m_prepare_ms = 0;
    std::chrono::steady_clock::time_point m_start;
    std::vector<const Points*>            m_subject, m_clip;
};
} // namespace Slic3r
#endif
