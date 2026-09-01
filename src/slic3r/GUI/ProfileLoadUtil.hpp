#ifndef slic3r_ProfileLoadUtil_hpp_
#define slic3r_ProfileLoadUtil_hpp_

#include <atomic>
#include <vector>
#include <nlohmann/json.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r { namespace GUI {

// Drives the per-file loops used while loading a vendor profile family.
//
// work(n, slot) runs for n in [0, nsize) on TBB workers; each invocation writes
// only its OWN slot, so there are no shared writes, no locks and no data races.
// A malformed item leaves its slot null (null == "skip this item"). destroy is
// checked per item so a cancel (the dialog was destroyed) unwinds promptly.
//
// Team convention - no exception control flow in this path: workers must use
// the non-throwing nlohmann/boost APIs (json::parse(..., nullptr, false) +
// is_discarded(), type-checked field access, error_code filesystem overloads),
// so a malformed file drops only itself. Anything that still escapes a worker
// (e.g. bad_alloc) propagates through parallel_for into the caller's catch.
template <typename WorkFn>
std::vector<nlohmann::json> parallel_load_items(int nsize, const std::atomic<bool> &destroy, WorkFn work)
{
    std::vector<nlohmann::json> slots(nsize);
    tbb::parallel_for(tbb::blocked_range<int>(0, nsize), [&](const tbb::blocked_range<int> &range) {
        for (int n = range.begin(); n != range.end(); ++n) {
            if (destroy.load(std::memory_order_acquire)) return;
            work(n, slots[n]);
        }
    });
    return slots;
}

// parallel_load_items + an in-order serial merge: runs work across the range,
// then calls merge(item) for every non-null slot in original list order
// (preserving ordering and first-wins dedup). The merge is skipped when destroy
// is set, so a destroyed dialog receives no partial writes; the caller should
// still re-check destroy afterwards to abort the remaining sections.
template <typename WorkFn, typename MergeFn>
void load_section(int nsize, const std::atomic<bool> &destroy, WorkFn work, MergeFn merge)
{
    auto slots = parallel_load_items(nsize, destroy, work);
    if (destroy.load(std::memory_order_acquire)) return;
    for (int n = 0; n < nsize; ++n)
        if (!slots[n].is_null()) merge(slots[n]);
}

}} // namespace Slic3r::GUI

#endif // slic3r_ProfileLoadUtil_hpp_
