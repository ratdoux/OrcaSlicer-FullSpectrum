#ifndef slic3r_FullSpectrum3mf_Writer_hpp_
#define slic3r_FullSpectrum3mf_Writer_hpp_

#include "Fs3mfTypes.hpp"

#include <string>
#include <memory>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
class Model;
class ModelObject;
class ModelVolume;
namespace ImageMap { struct VolumeData; }
}

namespace Slic3r::FullSpectrum3mf {

struct ObjectBindingInput
{
    int         model_object_id = 0;
    std::string stable_object_id;
};

struct VolumeBindingInput
{
    int              model_object_id = 0;
    int              model_volume_id = 0;
    std::string      stable_object_id;
    std::string      stable_volume_id;
    int              extruder_id = 0;
    std::vector<int> paint_states;
    std::shared_ptr<const ImageMap::VolumeData> image_map_data;
};

struct GeometryBindingInput
{
    std::string                     project_name;
    std::vector<ObjectBindingInput> objects;
    std::vector<VolumeBindingInput> volumes;
    std::vector<PreservedPart>      preserved_parts;
};

std::string stable_object_id_from_model_id(int model_object_id);
std::string stable_volume_id_from_model_id(int model_volume_id);
std::string stable_object_id_from_model(const Model &model, const ModelObject &object, int model_object_id);
std::string stable_volume_id_from_model(const Model &model, const ModelVolume &volume, int model_volume_id);
std::vector<PreservedPart> preserved_parts_from_model(const Model &model);

PackageModel build_package_model(const DynamicPrintConfig &config,
                                 const GeometryBindingInput &geometry,
                                 bool write_legacy_projection);

PackageWritePlan build_write_plan(const PackageModel &model);
PackageWritePlan build_write_plan(const DynamicPrintConfig &config,
                                  const GeometryBindingInput &geometry,
                                  bool write_legacy_projection);

FeatureNegotiationResult negotiate_features(const Manifest &manifest);
std::string relationships_xml(const std::vector<PackageRelationshipPlan> &relationships,
                              const std::string &id_prefix = "fs-rel-");

} // namespace Slic3r::FullSpectrum3mf

#endif
