#include "Contracts.hpp"

#include <tuple>

namespace Slic3r::MaterialBand {

bool RevisionKey::operator==(const RevisionKey& other) const
{
    return std::tie(object, geometry, paint, recipe, process, machine, generation) ==
           std::tie(other.object, other.geometry, other.paint, other.recipe, other.process, other.machine, other.generation);
}

const char* failure_name(FailureCode code)
{
    switch (code) {
    case FailureCode::UnsupportedTopology: return "UnsupportedTopology";
    case FailureCode::InfeasibleThickness: return "InfeasibleThickness";
    case FailureCode::IncompatibleOrder: return "IncompatibleOrder";
    case FailureCode::MaterialToleranceExceeded: return "MaterialToleranceExceeded";
    case FailureCode::ClearanceFailure: return "ClearanceFailure";
    case FailureCode::UnsupportedProfile: return "UnsupportedProfile";
    case FailureCode::NonConverged: return "NonConverged";
    case FailureCode::ResourceLimit: return "ResourceLimit";
    case FailureCode::InvalidInput: return "InvalidInput";
    case FailureCode::Cancelled: return "Cancelled";
    case FailureCode::StaleInput: return "StaleInput";
    }
    return "UnknownFailure";
}

} // namespace Slic3r::MaterialBand
