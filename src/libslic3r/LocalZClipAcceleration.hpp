#ifndef slic3r_LocalZClipAcceleration_hpp_
#define slic3r_LocalZClipAcceleration_hpp_

#include <functional>
#include <memory>

namespace Slic3r {
class ExtrusionEntity;
class ExtrusionEntityCollection;
class LocalZClipper;

// Opt-in, export-owned Vulkan context. Decisions borrow geometry only until
// begin_layer() and never replace seam placement or nonempty clipping results.
class LocalZClipAcceleration
{
public:
    static std::shared_ptr<LocalZClipAcceleration> from_environment();
    ~LocalZClipAcceleration();
    void begin_layer();
    void add(const ExtrusionEntityCollection& source, const LocalZClipper& masks, bool difference);
    void prepare(const std::function<void()>& cancel);
    bool proven_empty(const ExtrusionEntity& entity, const LocalZClipper* include_masks, const LocalZClipper* exclude_masks);
    bool validation() const;
    void report() const;

private:
    struct Impl;
    explicit LocalZClipAcceleration(bool validate);
    std::unique_ptr<Impl> m_impl;
};
} // namespace Slic3r
#endif
