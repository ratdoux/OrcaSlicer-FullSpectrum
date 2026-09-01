#include "ProjectSchemaVersion.hpp"

#include "Config.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

namespace {
constexpr ProjectSchemaDefinition PROJECT_SCHEMA {
    "project_schema_version",
    1,
    1
};
}

const ProjectSchemaDefinition& ProjectSchemaRegistry::definition()
{
    return PROJECT_SCHEMA;
}

int ProjectSchemaRegistry::version_from(const DynamicPrintConfig& config)
{
    if (const auto* option = config.option<ConfigOptionInt>(PROJECT_SCHEMA.config_key))
        return option->value;
    return PROJECT_SCHEMA.legacy_version;
}

bool ProjectSchemaRegistry::is_newer(const DynamicPrintConfig& config)
{
    return version_from(config) > PROJECT_SCHEMA.current_version;
}

} // namespace Slic3r
