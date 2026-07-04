#ifndef slic3r_MixedFilamentPreview_hpp_
#define slic3r_MixedFilamentPreview_hpp_

#include <string>
#include <vector>

namespace Slic3r {

enum class MixedFilamentSidewallBlendModel { Legacy, TdFilamentMixer, TdYuleNielsen };

struct MixedFilamentOpticalMaterial
{
    std::string color;
    double      td99_mm{0.0};
};

struct MixedFilamentSidewallSample
{
    int          layer_index{0};
    int          spatial_region_id{0};
    unsigned int filament_id{0};
    double       z_bottom_mm{0.0};
    double       height_mm{0.2};
};

struct MixedFilamentSidewallPredictionSettings
{
    MixedFilamentSidewallBlendModel blend_model{MixedFilamentSidewallBlendModel::Legacy};
    double                          yule_nielsen_n{2.0};
    double                          min_contribution{0.0005};
};

MixedFilamentSidewallBlendModel mixed_filament_sidewall_blend_model_from_string(const std::string& value);
std::string                     mixed_filament_sidewall_blend_model_to_string(MixedFilamentSidewallBlendModel model);

double mixed_filament_td99_transmittance(double thickness_mm, double td99_mm);
double mixed_filament_td99_opacity(double thickness_mm, double td99_mm);

bool mixed_filament_sidewall_prediction_available(const std::vector<unsigned int>& sequence,
                                                  const std::vector<double>&       physical_td99_mm,
                                                  MixedFilamentSidewallBlendModel  model);

std::vector<std::string> predict_mixed_filament_sidewall_sample_colors(const std::vector<MixedFilamentSidewallSample>&  samples,
                                                                       const std::vector<MixedFilamentOpticalMaterial>& materials,
                                                                       const MixedFilamentSidewallPredictionSettings&   settings);

std::vector<std::string> predict_mixed_filament_sequence_apparent_colors(const std::vector<unsigned int>&               sequence,
                                                                         const std::vector<std::string>&                physical_colors,
                                                                         const std::vector<double>&                     physical_td99_mm,
                                                                         double                                         layer_height_mm,
                                                                         const MixedFilamentSidewallPredictionSettings& settings);

std::string predict_mixed_filament_sequence_apparent_color_at(const std::vector<unsigned int>&               sequence,
                                                              size_t                                         target_index,
                                                              const std::vector<std::string>&                physical_colors,
                                                              const std::vector<double>&                     physical_td99_mm,
                                                              double                                         layer_height_mm,
                                                              const MixedFilamentSidewallPredictionSettings& settings,
                                                              const std::string&                             fallback);

std::string predict_mixed_filament_sequence_surface_color_at(const std::vector<unsigned int>&               sequence,
                                                             size_t                                         target_index,
                                                             const std::vector<std::string>&                physical_colors,
                                                             const std::vector<double>&                     physical_td99_mm,
                                                             double                                         layer_height_mm,
                                                             const MixedFilamentSidewallPredictionSettings& settings,
                                                             const std::string&                             fallback);

std::string predict_mixed_filament_sequence_surface_color_at(const std::vector<unsigned int>&               sequence,
                                                             const std::vector<double>&                     layer_heights_mm,
                                                             size_t                                         target_index,
                                                             const std::vector<std::string>&                physical_colors,
                                                             const std::vector<double>&                     physical_td99_mm,
                                                             double                                         layer_height_mm,
                                                             const MixedFilamentSidewallPredictionSettings& settings,
                                                             const std::string&                             fallback);

std::string predict_mixed_filament_sequence_aggregate_color(const std::vector<unsigned int>&               sequence,
                                                            const std::vector<std::string>&                physical_colors,
                                                            const std::vector<double>&                     physical_td99_mm,
                                                            double                                         layer_height_mm,
                                                            const MixedFilamentSidewallPredictionSettings& settings,
                                                            const std::string&                             fallback);

} // namespace Slic3r

#endif /* slic3r_MixedFilamentPreview_hpp_ */
