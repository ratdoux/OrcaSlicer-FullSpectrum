#ifndef slic3r_MaterialBand_Production_hpp_
#define slic3r_MaterialBand_Production_hpp_

#include "FinalPrintPlan.hpp"

namespace Slic3r {
class Print;
class DynamicPrintConfig;
namespace MaterialBand {

struct ProductionResult
{
    RevisionKey                           revision;
    std::shared_ptr<const FinalPrintPlan> plan;
    std::optional<Failure>                fallback;
    std::vector<std::string>              unsupported_settings;
    size_t                                object_count = 0, instance_count = 0;
    size_t                                geometry_reuses = 0;
    double                                snapshot_ms = 0., paths_ms = 0., finalize_ms = 0.;
    bool                                  used_material_bands() const { return bool(plan); }
    std::string                           fallback_message() const
    {
        if (unsupported_settings.empty())
            return fallback ? fallback->detail : std::string();
        std::string message;
        for (const auto& setting : unsupported_settings) {
            if (!message.empty())
                message += "; ";
            message += setting;
        }
        return message;
    }
};

bool enabled(const DynamicPrintConfig& config);
bool is_engine_option(const std::string& key);

// Immutable print-wide acceptance. An unsupported object, profile, purge,
// support or clearance condition rejects the complete scope before legacy or
// new canonical material paths are published.
std::shared_ptr<const ProductionResult> plan_production_print(const Print&                 print,
                                                              const RevisionKey&           revision,
                                                              const std::function<bool()>& cancelled = {},
                                                              const std::function<bool()>& stale     = {});

} // namespace MaterialBand
} // namespace Slic3r
#endif
