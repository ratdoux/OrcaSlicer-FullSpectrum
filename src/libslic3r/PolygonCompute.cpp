#include "PolygonCompute.hpp"
#include "Compute/LocalZClipDecision.hpp"
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace Slic3r {
namespace {
using Stage                      = PolygonComputeScope::Stage;
using Clock                      = std::chrono::steady_clock;
thread_local Stage current_stage = Stage::None;
double             milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
const char*        stage_name(Stage s)
{
    return s == Stage::Walls ? "walls" : s == Stage::Shells ? "shells" : s == Stage::Paint ? "paint" : "none";
}
struct Statistics
{
    uint64_t calls = 0, empty = 0, eligible = 0, eligible_empty = 0, gpu = 0, proven = 0, skipped = 0, captured = 0;
    double   cpu_ms = 0, empty_cpu_ms = 0, eligible_cpu_ms = 0, eligible_empty_cpu_ms = 0, prepare_ms = 0;
};
struct State;
State* initialized_state = nullptr;
struct State
{
    std::string                               mode, stages, profile;
    std::mutex                                gpu_mutex, stats_mutex;
    std::unique_ptr<VulkanLocalZClipDecision> backend;
    bool                                      disabled = false;
    Statistics                                stats[4];
    std::ofstream                             capture;
    size_t                                    capture_bytes = 0;
    State()
    {
        if (const char* value = std::getenv("ORCA_POLYGON_COMPUTE"))
            mode = value;
        if (mode.empty())
            mode = "cpu";
        if (mode != "cpu" && mode != "profile" && mode != "vulkan" && mode != "validate")
            throw std::runtime_error("ORCA_POLYGON_COMPUTE must be cpu, profile, vulkan, or validate");
        if (const char* value = std::getenv("ORCA_POLYGON_STAGES"))
            stages = value;
        if (const char* value = std::getenv("ORCA_POLYGON_PROFILE"))
            profile = value;
        if (const char* value = std::getenv("ORCA_POLYGON_CAPTURE"); value && *value) {
            capture.open(std::filesystem::u8path(value), std::ios::binary);
            if (!capture)
                throw std::runtime_error("Cannot open polygon capture");
        }
        initialized_state = this;
    }
};
State& state()
{
    static State value;
    return value;
}
size_t edge_count(const std::vector<const Points*>& paths)
{
    size_t count = 0;
    for (const Points* p : paths)
        count += p->size();
    return count;
}
bool append_edges(const std::vector<const Points*>& paths, std::vector<ClipMaskEdge>& edges)
{
    for (const Points* p : paths) {
        if (p->size() < 3)
            return false;
        for (size_t i = 0; i < p->size(); ++i) {
            const Point& a = (*p)[i];
            const Point& b = (*p)[(i + 1) % p->size()];
            for (coord_t v : {a.x(), a.y(), b.x(), b.y()})
                if (v < -(1 << 29) || v > (1 << 29))
                    return false;
            edges.push_back({int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())});
        }
    }
    return true;
}
nlohmann::json encode(const std::vector<const Points*>& paths)
{
    auto result = nlohmann::json::array();
    for (const Points* path : paths) {
        auto points = nlohmann::json::array();
        for (const Point& p : *path)
            points.push_back({p.x(), p.y()});
        result.push_back(std::move(points));
    }
    return result;
}
} // namespace
PolygonComputeScope::PolygonComputeScope(Stage stage) : m_previous(current_stage) { current_stage = stage; }
PolygonComputeScope::~PolygonComputeScope() { current_stage = m_previous; }
PolygonComputeQuery::PolygonComputeQuery(ClipperLib::ClipType operation, ClipperLib::PolyFillType fill)
    : m_stage(current_stage), m_operation(operation), m_fill(fill)
{
    if (m_stage == Stage::None)
        return;
    auto& s    = state();
    m_active   = s.mode != "cpu" && (s.stages.empty() || s.stages.find(stage_name(m_stage)) != std::string::npos);
    m_validate = s.mode == "validate";
}
void PolygonComputeQuery::classify()
{
    auto&      s     = state();
    const auto start = Clock::now();
    m_subject_edges  = edge_count(m_subject);
    m_clip_edges     = edge_count(m_clip);
    // Bound arithmetic work as well as buffers; this is not a general GPU Boolean.
    const uint64_t work = uint64_t(m_subject_edges) * m_clip_edges;
    m_eligible          = m_fill == ClipperLib::pftNonZero &&
                 (m_operation == ClipperLib::ctIntersection || m_operation == ClipperLib::ctDifference) && m_subject_edges &&
                 m_clip_edges && m_subject_edges + m_clip_edges <= 16384 && work >= (1u << 18) && work <= (1u << 23);
    if (m_eligible && s.mode != "profile") {
        std::vector<ClipMaskEdge> edges;
        edges.reserve(m_subject_edges + m_clip_edges);
        if (!append_edges(m_subject, edges) || !append_edges(m_clip, edges)) {
            m_eligible = false;
        } else {
            auto queries = closed_polygon_empty_queries(edges, m_subject_edges, m_operation == ClipperLib::ctDifference);
            std::lock_guard<std::mutex> lock(s.gpu_mutex);
            std::string                 error;
            if (!s.disabled && !s.backend) {
                const char* filter = std::getenv("ORCA_VULKAN_DEVICE");
                s.backend          = VulkanLocalZClipDecision::create(std::vector<ClipMaskEdge>(16384), filter ? filter : "", error);
                s.disabled         = !s.backend;
            }
            std::vector<uint32_t> decisions;
            if (!s.disabled && s.backend->update_edges(edges, error) && s.backend->compute(queries, decisions, error)) {
                m_gpu    = true;
                m_proven = std::all_of(decisions.begin(), decisions.end(), [](uint32_t v) { return v == 1; });
            } else if (!error.empty()) {
                s.disabled = true;
                BOOST_LOG_TRIVIAL(warning) << "Polygon Vulkan fallback: " << error;
            }
            if (s.disabled && m_validate)
                throw std::runtime_error("Polygon Vulkan validation unavailable");
        }
    }
    m_prepare_ms = milliseconds(start);
    m_start      = Clock::now();
}
bool PolygonComputeQuery::skip() const { return m_proven && !m_validate; }
void PolygonComputeQuery::finish(bool empty, bool skipped)
{
    if (!m_active)
        return;
    const double cpu_ms = skipped ? 0 : milliseconds(m_start);
    if (m_validate && m_proven && !empty)
        throw std::runtime_error("Polygon Vulkan rejected nonempty geometry");
    auto&                       s = state();
    std::lock_guard<std::mutex> lock(s.stats_mutex);
    auto&                       c = s.stats[size_t(m_stage)];
    ++c.calls;
    c.empty += empty;
    c.eligible += m_eligible;
    c.eligible_empty += m_eligible && empty;
    c.gpu += m_gpu;
    c.proven += m_proven;
    c.skipped += skipped;
    c.cpu_ms += cpu_ms;
    c.empty_cpu_ms += empty ? cpu_ms : 0;
    c.eligible_cpu_ms += m_eligible ? cpu_ms : 0;
    c.eligible_empty_cpu_ms += m_eligible && empty ? cpu_ms : 0;
    c.prepare_ms += m_prepare_ms;
    if (s.capture.is_open() && m_eligible && c.eligible % 127 == 1 && s.capture_bytes < 64u * 1024 * 1024) {
        nlohmann::json row  = {{"stage", stage_name(m_stage)}, {"operation", int(m_operation)}, {"empty", empty}, {"cpu_ms", cpu_ms},
                               {"subject", encode(m_subject)}, {"clip", encode(m_clip)}};
        std::string    data = row.dump();
        if (s.capture_bytes + data.size() + 1 <= 64u * 1024 * 1024) {
            s.capture << data << '\n';
            s.capture_bytes += data.size() + 1;
            ++c.captured;
            if (!s.capture)
                throw std::runtime_error("Failed to write polygon capture");
        }
    }
}
void PolygonComputeScope::shutdown()
{
    if (!initialized_state)
        return;
    auto&          s = *initialized_state;
    nlohmann::json report;
    for (Stage stage : {Stage::Walls, Stage::Shells, Stage::Paint}) {
        const auto& c             = s.stats[size_t(stage)];
        report[stage_name(stage)] = {{"calls", c.calls},
                                     {"empty", c.empty},
                                     {"eligible", c.eligible},
                                     {"eligible_empty", c.eligible_empty},
                                     {"gpu", c.gpu},
                                     {"proven", c.proven},
                                     {"skipped", c.skipped},
                                     {"captured", c.captured},
                                     {"cpu_ms", c.cpu_ms},
                                     {"empty_cpu_ms", c.empty_cpu_ms},
                                     {"eligible_cpu_ms", c.eligible_cpu_ms},
                                     {"eligible_empty_cpu_ms", c.eligible_empty_cpu_ms},
                                     {"prepare_ms", c.prepare_ms}};
        if (c.calls)
            BOOST_LOG_TRIVIAL(info) << "Polygon compute stage=" << stage_name(stage) << " " << report[stage_name(stage)].dump();
    }
    if (!s.profile.empty()) {
        std::ofstream output(std::filesystem::u8path(s.profile));
        output << report.dump(2) << '\n';
        if (!output)
            std::fprintf(stderr, "Failed to write polygon compute report\n");
    }
    if (s.capture.is_open())
        s.capture.close();
    s.backend.reset();
}
} // namespace Slic3r
