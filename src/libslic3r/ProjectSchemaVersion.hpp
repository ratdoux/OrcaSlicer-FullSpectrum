#pragma once

#include <string>

namespace Slic3r {

class DynamicPrintConfig;

struct ProjectSchemaDefinition
{
    const char* config_key;
    int         current_version;
    int         legacy_version;
};

class ProjectSchemaRegistry
{
public:
    static const ProjectSchemaDefinition& definition();
    static int version_from(const DynamicPrintConfig& config);
    static bool is_newer(const DynamicPrintConfig& config);
};

} // namespace Slic3r
