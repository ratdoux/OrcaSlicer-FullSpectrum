#include "SimplePmCalibration.hpp"

#include "../Utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

namespace Slic3r::ImageMap {
namespace {

constexpr float FINDER_SIZE_MM = 6.f;
constexpr float FINDER_INSET_MM = 1.f;
// Keep every printed identity module comfortably larger than the Simple PM
// transition footprint. A taller, lower-density layout is easier to recover
// from a photographed perimeter-modulated wall than the former 0.55 mm strip.
constexpr size_t HEADER_COLUMNS = 16;
constexpr size_t HEADER_ROWS = 8;
constexpr float HEADER_CELL_MM = 2.f;
constexpr float HEADER_TOP_INSET_MM = 2.f;
constexpr size_t HEADER_BYTES = HEADER_COLUMNS * HEADER_ROWS / 8;
constexpr uint16_t HEADER_MAGIC = 0x4653; // "FS"
constexpr size_t NEAREST_PROFILE_SAMPLES = 6;

std::mutex                                                    g_profile_mutex;
std::unordered_map<uint64_t, std::optional<SimplePmCalibrationProfile>> g_profile_cache;
std::atomic<uint64_t>                                         g_profile_revision{1};

float clamp01(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
}

void hash_bytes(uint64_t& hash, const void* data, size_t size)
{
    constexpr uint64_t prime = 1099511628211ull;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= uint64_t(bytes[index]);
        hash *= prime;
    }
}

void hash_string(uint64_t& hash, const std::string& value)
{
    hash_bytes(hash, value.data(), value.size());
    const uint8_t terminator = 0;
    hash_bytes(hash, &terminator, sizeof(terminator));
}

uint8_t color_byte(float value)
{
    return uint8_t(std::lround(clamp01(value) * 255.f));
}

RGBA byte_quantized(const RGBA& color)
{
    return RGBA{float(color_byte(color[0])) / 255.f, float(color_byte(color[1])) / 255.f,
                float(color_byte(color[2])) / 255.f, 1.f};
}

float luminance(const RGBA& color)
{
    return 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
}

uint16_t crc16(const uint8_t* bytes, size_t count)
{
    uint16_t crc = 0xffffu;
    for (size_t index = 0; index < count; ++index) {
        crc ^= uint16_t(bytes[index]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000u) != 0 ? uint16_t((crc << 1) ^ 0x1021u) : uint16_t(crc << 1);
    }
    return crc;
}

std::array<uint8_t, HEADER_BYTES> header_bytes(uint64_t signature, size_t component_count, int total_units)
{
    std::array<uint8_t, HEADER_BYTES> bytes{};
    bytes[0] = uint8_t(HEADER_MAGIC >> 8);
    bytes[1] = uint8_t(HEADER_MAGIC & 0xffu);
    bytes[2] = uint8_t(SIMPLE_PM_CALIBRATION_SCHEMA_VERSION);
    bytes[3] = uint8_t(std::min<size_t>(component_count, 255));
    bytes[4] = uint8_t(std::clamp(total_units, 0, 255));
    bytes[5] = uint8_t(signature >> 56);
    bytes[6] = uint8_t(signature >> 48);
    bytes[7] = uint8_t(signature >> 40);
    bytes[8] = uint8_t(signature >> 32);
    bytes[9] = uint8_t(signature >> 24);
    bytes[10] = uint8_t(signature >> 16);
    bytes[11] = uint8_t(signature >> 8);
    bytes[12] = uint8_t(signature);
    const uint16_t check = crc16(bytes.data(), 13);
    bytes[13] = uint8_t(check >> 8);
    bytes[14] = uint8_t(check & 0xffu);
    bytes[15] = uint8_t(0xa5u);
    return bytes;
}

struct RasterPainter
{
    TextureAsset& asset;

    int px(float x_mm, float width_mm) const
    {
        return std::clamp(int(std::lround(x_mm / width_mm * float(asset.width))), 0, int(asset.width));
    }

    int py(float y_mm, float height_mm) const
    {
        return std::clamp(int(std::lround(y_mm / height_mm * float(asset.height))), 0, int(asset.height));
    }

    void fill_pixels(int x0, int y0, int x1, int y1, const RGBA& color)
    {
        x0 = std::clamp(x0, 0, int(asset.width));
        x1 = std::clamp(x1, 0, int(asset.width));
        y0 = std::clamp(y0, 0, int(asset.height));
        y1 = std::clamp(y1, 0, int(asset.height));
        if (x1 <= x0 || y1 <= y0)
            return;
        const std::array<uint8_t, 4> rgba = {color_byte(color[0]), color_byte(color[1]), color_byte(color[2]), color_byte(color[3])};
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const size_t offset = (size_t(y) * size_t(asset.width) + size_t(x)) * 4;
                std::copy(rgba.begin(), rgba.end(), asset.rgba.begin() + offset);
            }
        }
    }

    void fill_mm(float x0, float y0, float x1, float y1, float width_mm, float height_mm, const RGBA& color)
    {
        fill_pixels(px(x0, width_mm), py(y0, height_mm), px(x1, width_mm), py(y1, height_mm), color);
    }
};

void draw_finder(RasterPainter& painter,
                 float center_x,
                 float center_y,
                 float plaque_width,
                 float plaque_height,
                 const RGBA& dark,
                 const RGBA& light)
{
    const float half = 0.5f * FINDER_SIZE_MM;
    painter.fill_mm(center_x - half, center_y - half, center_x + half, center_y + half,
                    plaque_width, plaque_height, dark);
    painter.fill_mm(center_x - half + FINDER_INSET_MM, center_y - half + FINDER_INSET_MM,
                    center_x + half - FINDER_INSET_MM, center_y + half - FINDER_INSET_MM,
                    plaque_width, plaque_height, light);
    painter.fill_mm(center_x - half + 2.f * FINDER_INSET_MM, center_y - half + 2.f * FINDER_INSET_MM,
                    center_x + half - 2.f * FINDER_INSET_MM, center_y + half - 2.f * FINDER_INSET_MM,
                    plaque_width, plaque_height, dark);
}

void draw_header(RasterPainter& painter,
                 const std::array<uint8_t, HEADER_BYTES>& bytes,
                 const SimplePmCalibrationChartSettings& settings,
                 const RGBA& dark,
                 const RGBA& light)
{
    const float width = float(HEADER_COLUMNS) * HEADER_CELL_MM;
    const float height = float(HEADER_ROWS) * HEADER_CELL_MM;
    const float origin_x = 0.5f * (settings.plaque_width_mm - width);
    const float origin_y = settings.plaque_height_mm - HEADER_TOP_INSET_MM - height;
    for (size_t bit_index = 0; bit_index < HEADER_COLUMNS * HEADER_ROWS; ++bit_index) {
        const size_t row = bit_index / HEADER_COLUMNS;
        const size_t column = bit_index % HEADER_COLUMNS;
        const bool bit = (bytes[bit_index / 8] & uint8_t(0x80u >> (bit_index % 8))) != 0;
        const float x0 = origin_x + float(column) * HEADER_CELL_MM;
        const float y0 = origin_y + float(HEADER_ROWS - row - 1) * HEADER_CELL_MM;
        painter.fill_mm(x0, y0, x0 + HEADER_CELL_MM, y0 + HEADER_CELL_MM,
                        settings.plaque_width_mm, settings.plaque_height_mm, bit ? dark : light);
    }
}

struct RecipeCandidate
{
    std::vector<uint8_t> units;
    RGBA                 target{0.f, 0.f, 0.f, 1.f};
    RGBA                 predicted{0.f, 0.f, 0.f, 1.f};
};

std::string recipe_key(const std::vector<uint8_t>& units)
{
    return std::string(reinterpret_cast<const char*>(units.data()), units.size());
}

std::vector<RecipeCandidate> printable_recipes(const ContinuousColorSolver& printable_solver,
                                               const ContinuousColorSolver& raw_solver)
{
    std::vector<RecipeCandidate> recipes;
    std::unordered_set<std::string> seen;
    const int total_units = continuous_color_solver_total_units(printable_solver.component_count());
    recipes.reserve(printable_solver.candidate_count());
    seen.reserve(printable_solver.candidate_count());

    for (size_t candidate_index = 0; candidate_index < printable_solver.candidate_count(); ++candidate_index) {
        const std::optional<ContinuousColorCandidate> candidate = printable_solver.candidate(candidate_index);
        if (!candidate)
            continue;
        const RGBA target = byte_quantized(candidate->predicted_color);
        const std::vector<double> actual_weights = printable_solver.solve(target);
        if (actual_weights.size() != printable_solver.component_count())
            continue;

        std::vector<uint8_t> units(actual_weights.size(), 0);
        int assigned = 0;
        size_t strongest = 0;
        for (size_t component_index = 0; component_index < actual_weights.size(); ++component_index) {
            const int unit = std::clamp(int(std::lround(actual_weights[component_index] * double(total_units))), 0, total_units);
            units[component_index] = uint8_t(unit);
            assigned += unit;
            if (actual_weights[component_index] > actual_weights[strongest])
                strongest = component_index;
        }
        if (assigned != total_units && !units.empty())
            units[strongest] = uint8_t(std::clamp(int(units[strongest]) + total_units - assigned, 0, total_units));
        if (!seen.emplace(recipe_key(units)).second)
            continue;

        std::vector<double> normalized(units.size(), 0.0);
        for (size_t component_index = 0; component_index < units.size(); ++component_index)
            normalized[component_index] = double(units[component_index]) / double(total_units);
        // Measurements train the residual against the uncalibrated KM/K-S
        // prediction for the recipe that will actually print. The authored
        // target comes from the currently active solver, so regenerating a
        // chart while replacing an older profile cannot silently change the
        // recipe selected by the slice-time lookup.
        const std::optional<RGBA> predicted = raw_solver.predict_weights(normalized);
        recipes.push_back({std::move(units), target, predicted.value_or(candidate->predicted_color)});
    }
    return recipes;
}

double recipe_distance_squared(const RecipeCandidate& lhs, const RecipeCandidate& rhs, int total_units)
{
    double result = 0.0;
    for (size_t index = 0; index < lhs.units.size() && index < rhs.units.size(); ++index) {
        const double delta = (double(lhs.units[index]) - double(rhs.units[index])) / double(total_units);
        result += delta * delta;
    }
    return result;
}

std::vector<size_t> choose_recipe_indices(const std::vector<RecipeCandidate>& recipes, size_t capacity, int total_units)
{
    if (recipes.size() <= capacity) {
        std::vector<size_t> result(recipes.size());
        std::iota(result.begin(), result.end(), size_t(0));
        return result;
    }

    std::vector<size_t> selected;
    std::vector<bool> used(recipes.size(), false);
    std::vector<double> minimum_distance(recipes.size(), std::numeric_limits<double>::infinity());
    selected.reserve(capacity);

    auto select = [&](size_t index) {
        if (index >= recipes.size() || used[index] || selected.size() >= capacity)
            return;
        used[index] = true;
        selected.push_back(index);
        for (size_t candidate_index = 0; candidate_index < recipes.size(); ++candidate_index) {
            if (!used[candidate_index])
                minimum_distance[candidate_index] =
                    std::min(minimum_distance[candidate_index],
                             recipe_distance_squared(recipes[index], recipes[candidate_index], total_units));
        }
    };

    // Preserve every solid component as a camera-normalization anchor.
    for (size_t component_index = 0; component_index < recipes.front().units.size(); ++component_index) {
        const auto found = std::find_if(recipes.begin(), recipes.end(), [&](const RecipeCandidate& recipe) {
            return recipe.units[component_index] == total_units &&
                   std::accumulate(recipe.units.begin(), recipe.units.end(), 0) == total_units;
        });
        if (found != recipes.end())
            select(size_t(std::distance(recipes.begin(), found)));
    }

    // Also seed the center of the simplex; it stabilizes interpolation in the
    // most frequently used four-way mixture region.
    size_t center_index = 0;
    double center_error = std::numeric_limits<double>::infinity();
    const double ideal = 1.0 / double(recipes.front().units.size());
    for (size_t index = 0; index < recipes.size(); ++index) {
        double error = 0.0;
        for (uint8_t unit : recipes[index].units) {
            const double delta = double(unit) / double(total_units) - ideal;
            error += delta * delta;
        }
        if (error < center_error) {
            center_error = error;
            center_index = index;
        }
    }
    select(center_index);

    while (selected.size() < capacity) {
        size_t best = recipes.size();
        double best_distance = -1.0;
        for (size_t index = 0; index < recipes.size(); ++index) {
            if (!used[index] && minimum_distance[index] > best_distance) {
                best_distance = minimum_distance[index];
                best = index;
            }
        }
        if (best >= recipes.size())
            break;
        select(best);
    }

    // Lexicographic recipe ordering makes adjacent cells change gradually,
    // reducing the amount of work left for each guard gutter.
    std::sort(selected.begin(), selected.end(), [&](size_t lhs, size_t rhs) {
        return recipes[lhs].units < recipes[rhs].units;
    });
    return selected;
}

std::string profile_path(uint64_t signature)
{
    std::ostringstream filename;
    filename << "simple-pm-" << std::hex << std::setw(16) << std::setfill('0') << signature << ".json";
    return (boost::filesystem::path(data_dir()) / "fullspectrum" / "color_calibration" / filename.str()).string();
}

nlohmann::json color_json(const RGBA& color)
{
    return nlohmann::json::array({color[0], color[1], color[2], color[3]});
}

bool parse_color(const nlohmann::json& json, RGBA& color)
{
    if (!json.is_array() || json.size() < 3)
        return false;
    for (size_t channel = 0; channel < 4; ++channel)
        color[channel] = channel < json.size() && json[channel].is_number() ? clamp01(json[channel].get<float>()) : 1.f;
    return true;
}

std::optional<SimplePmCalibrationProfile> load_profile_file(uint64_t signature)
{
    boost::nowide::ifstream input(profile_path(signature));
    if (!input.is_open())
        return std::nullopt;
    const nlohmann::json root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return std::nullopt;

    SimplePmCalibrationProfile profile;
    profile.schema_version = root.value("schema_version", 0u);
    profile.signature = root.value("signature", uint64_t(0));
    profile.color_mix_model = ColorMixModel(root.value("color_mix_model", 0));
    profile.total_units = root.value("total_units", 0);
    if (profile.schema_version != SIMPLE_PM_CALIBRATION_SCHEMA_VERSION || profile.signature != signature || profile.total_units <= 0)
        return std::nullopt;

    const nlohmann::json components = root.value("components", nlohmann::json::array());
    for (const nlohmann::json& item : components) {
        if (!item.is_object())
            return std::nullopt;
        ContinuousColorComponent component;
        component.color_hex = item.value("color_hex", std::string());
        if (item.contains("td_mm") && item["td_mm"].is_number())
            component.transmission_distance_mm = item["td_mm"].get<double>();
        if (item.contains("material_id") && item["material_id"].is_string())
            component.material_id = item["material_id"].get<std::string>();
        profile.components.emplace_back(std::move(component));
    }

    const nlohmann::json observations = root.value("observations", nlohmann::json::array());
    for (const nlohmann::json& item : observations) {
        if (!item.is_object())
            continue;
        SimplePmCalibrationObservation observation;
        observation.component_units = item.value("units", std::vector<uint8_t>());
        observation.confidence = std::clamp(item.value("confidence", 0.f), 0.f, 1.f);
        if (observation.component_units.size() != profile.components.size() ||
            !parse_color(item.value("expected", nlohmann::json::array()), observation.expected_color) ||
            !parse_color(item.value("measured", nlohmann::json::array()), observation.measured_color))
            continue;
        profile.observations.emplace_back(std::move(observation));
    }
    if (profile.components.size() < 2 || profile.observations.size() < profile.components.size())
        return std::nullopt;
    return profile;
}

std::optional<SimplePmCalibrationProfile> matching_profile(uint64_t signature)
{
    std::lock_guard<std::mutex> lock(g_profile_mutex);
    const auto cached = g_profile_cache.find(signature);
    if (cached != g_profile_cache.end())
        return cached->second;
    std::optional<SimplePmCalibrationProfile> loaded = load_profile_file(signature);
    g_profile_cache.emplace(signature, loaded);
    return loaded;
}

cv::Mat top_down_rgba(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height)
{
    cv::Mat image(int(height), int(width), CV_8UC4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = rgba.data() + size_t(height - y - 1) * size_t(width) * 4;
        std::copy(source, source + size_t(width) * 4, image.ptr<uint8_t>(int(y)));
    }
    return image;
}

int contour_depth(const std::vector<cv::Vec4i>& hierarchy, int index)
{
    int depth = 0;
    int child = index >= 0 && size_t(index) < hierarchy.size() ? hierarchy[size_t(index)][2] : -1;
    while (child >= 0 && size_t(child) < hierarchy.size() && depth < 8) {
        ++depth;
        child = hierarchy[size_t(child)][2];
    }
    return depth;
}

struct FinderCandidate
{
    cv::Point2f center;
    float       area{0.f};
    float       score{0.f};
};

std::vector<cv::Point2f> detect_finders(const cv::Mat& rgba, float& confidence)
{
    confidence = 0.f;
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0.8);
    cv::Mat thresholded;
    const int block = std::max(15, (std::min(gray.cols, gray.rows) / 30) | 1);
    cv::adaptiveThreshold(gray, thresholded, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, block, 3.0);

    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(thresholded, contours, hierarchy, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);
    std::vector<FinderCandidate> candidates;
    const double image_area = double(gray.cols) * double(gray.rows);
    for (size_t index = 0; index < contours.size(); ++index) {
        const double area = std::abs(cv::contourArea(contours[index]));
        if (area < image_area * 0.00005 || area > image_area * 0.08)
            continue;
        std::vector<cv::Point> polygon;
        cv::approxPolyDP(contours[index], polygon, 0.04 * cv::arcLength(contours[index], true), true);
        if (polygon.size() != 4 || !cv::isContourConvex(polygon))
            continue;
        const cv::Rect box = cv::boundingRect(polygon);
        const float aspect = box.height > 0 ? float(box.width) / float(box.height) : 0.f;
        if (aspect < 0.62f || aspect > 1.62f)
            continue;
        const int depth = contour_depth(hierarchy, int(index));
        if (depth < 2)
            continue;
        const cv::Moments moments = cv::moments(polygon);
        if (std::abs(moments.m00) <= 1e-6)
            continue;
        FinderCandidate candidate;
        candidate.center = cv::Point2f(float(moments.m10 / moments.m00), float(moments.m01 / moments.m00));
        candidate.area = float(area);
        candidate.score = float(area) * float(1 + depth * depth);
        candidates.emplace_back(candidate);
    }
    std::sort(candidates.begin(), candidates.end(), [](const FinderCandidate& lhs, const FinderCandidate& rhs) {
        return lhs.score > rhs.score;
    });

    std::vector<FinderCandidate> unique;
    const float separation = 0.035f * float(std::min(gray.cols, gray.rows));
    for (const FinderCandidate& candidate : candidates) {
        if (std::none_of(unique.begin(), unique.end(), [&](const FinderCandidate& existing) {
                return cv::norm(existing.center - candidate.center) < separation;
            }))
            unique.emplace_back(candidate);
        if (unique.size() >= 12)
            break;
    }
    if (unique.size() < 4)
        return {};

    // The four finder patterns are the extreme points of their own bounding
    // quadrilateral. Pick one candidate for each image corner.
    auto pick = [&](auto score_fn, bool maximum, const std::vector<size_t>& excluded) {
        size_t best = unique.size();
        float best_score = maximum ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
        for (size_t index = 0; index < unique.size(); ++index) {
            if (std::find(excluded.begin(), excluded.end(), index) != excluded.end())
                continue;
            const float score = score_fn(unique[index].center);
            if ((maximum && score > best_score) || (!maximum && score < best_score)) {
                best = index;
                best_score = score;
            }
        }
        return best;
    };
    std::vector<size_t> chosen;
    const size_t tl = pick([](const cv::Point2f& point) { return point.x + point.y; }, false, chosen);
    chosen.push_back(tl);
    const size_t br = pick([](const cv::Point2f& point) { return point.x + point.y; }, true, chosen);
    chosen.push_back(br);
    const size_t tr = pick([](const cv::Point2f& point) { return point.x - point.y; }, true, chosen);
    chosen.push_back(tr);
    const size_t bl = pick([](const cv::Point2f& point) { return point.x - point.y; }, false, chosen);
    if (tl >= unique.size() || tr >= unique.size() || br >= unique.size() || bl >= unique.size())
        return {};

    const std::array<float, 4> areas = {unique[tl].area, unique[tr].area, unique[br].area, unique[bl].area};
    const float minimum_area = *std::min_element(areas.begin(), areas.end());
    const float maximum_area = *std::max_element(areas.begin(), areas.end());
    confidence = maximum_area > 0.f ? std::clamp(minimum_area / maximum_area, 0.f, 1.f) : 0.f;
    return {unique[tl].center, unique[tr].center, unique[br].center, unique[bl].center};
}

cv::Point2f chart_point_top_down(float x_mm, float y_mm, const SimplePmCalibrationChartSettings& settings)
{
    return cv::Point2f(x_mm / settings.plaque_width_mm * float(settings.texture_width - 1),
                       (1.f - y_mm / settings.plaque_height_mm) * float(settings.texture_height - 1));
}

std::array<uint8_t, 3> median_patch_rgb(const cv::Mat& warped, const std::array<float, 4>& uv_rect, float& confidence)
{
    const float inset = 0.28f;
    const float u0 = uv_rect[0] + (uv_rect[2] - uv_rect[0]) * inset;
    const float u1 = uv_rect[2] - (uv_rect[2] - uv_rect[0]) * inset;
    const float v0 = uv_rect[1] + (uv_rect[3] - uv_rect[1]) * inset;
    const float v1 = uv_rect[3] - (uv_rect[3] - uv_rect[1]) * inset;
    const int x0 = std::clamp(int(std::floor(u0 * float(warped.cols))), 0, warped.cols - 1);
    const int x1 = std::clamp(int(std::ceil(u1 * float(warped.cols))), x0 + 1, warped.cols);
    const int y0 = std::clamp(int(std::floor((1.f - v1) * float(warped.rows))), 0, warped.rows - 1);
    const int y1 = std::clamp(int(std::ceil((1.f - v0) * float(warped.rows))), y0 + 1, warped.rows);

    std::array<std::vector<uint8_t>, 3> channels;
    const size_t count = size_t(x1 - x0) * size_t(y1 - y0);
    for (auto& channel : channels)
        channel.reserve(count);
    double sum_luminance = 0.0;
    double sum_luminance_squared = 0.0;
    size_t clipped = 0;
    for (int y = y0; y < y1; ++y) {
        const cv::Vec4b* row = warped.ptr<cv::Vec4b>(y);
        for (int x = x0; x < x1; ++x) {
            const cv::Vec4b pixel = row[x];
            for (size_t channel = 0; channel < 3; ++channel)
                channels[channel].push_back(pixel[channel]);
            const double value = (double(pixel[0]) + double(pixel[1]) + double(pixel[2])) / (3.0 * 255.0);
            sum_luminance += value;
            sum_luminance_squared += value * value;
            clipped += pixel[0] <= 2 || pixel[1] <= 2 || pixel[2] <= 2 || pixel[0] >= 253 || pixel[1] >= 253 || pixel[2] >= 253;
        }
    }
    std::array<uint8_t, 3> median{};
    for (size_t channel = 0; channel < 3; ++channel) {
        auto middle = channels[channel].begin() + channels[channel].size() / 2;
        std::nth_element(channels[channel].begin(), middle, channels[channel].end());
        median[channel] = *middle;
    }
    const double divisor = std::max<size_t>(count, 1);
    const double mean = sum_luminance / divisor;
    const double variance = std::max(0.0, sum_luminance_squared / divisor - mean * mean);
    const float uniformity = 1.f - std::clamp(float(std::sqrt(variance) / 0.12), 0.f, 1.f);
    const float unclipped = 1.f - std::clamp(float(double(clipped) / divisor), 0.f, 1.f);
    confidence = uniformity * unclipped;
    return median;
}

std::array<std::pair<double, double>, 3> fit_camera_channels(
    const std::vector<std::pair<RGBA, std::array<uint8_t, 3>>>& anchors)
{
    std::array<std::pair<double, double>, 3> transforms{{{1.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}}};
    if (anchors.size() < 2)
        return transforms;
    for (size_t channel = 0; channel < 3; ++channel) {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (const auto& anchor : anchors) {
            const double x = double(anchor.second[channel]) / 255.0;
            const double y = double(anchor.first[channel]);
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double n = double(anchors.size());
        const double denominator = n * sxx - sx * sx;
        if (std::abs(denominator) <= 1e-8)
            continue;
        const double gain = std::clamp((n * sxy - sx * sy) / denominator, 0.45, 2.2);
        const double offset = std::clamp((sy - gain * sx) / n, -0.30, 0.30);
        transforms[channel] = {gain, offset};
    }
    return transforms;
}

RGBA normalized_photo_color(const std::array<uint8_t, 3>& rgb,
                            const std::array<std::pair<double, double>, 3>& transforms)
{
    RGBA result{0.f, 0.f, 0.f, 1.f};
    for (size_t channel = 0; channel < 3; ++channel)
        result[channel] = clamp01(float(transforms[channel].first * (double(rgb[channel]) / 255.0) + transforms[channel].second));
    return result;
}

bool header_matches(const cv::Mat& warped,
                    const SimplePmCalibrationChart& chart)
{
    const std::array<uint8_t, HEADER_BYTES> expected =
        header_bytes(chart.signature, chart.patches.empty() ? 0 : chart.patches.front().component_units.size(), chart.total_units);
    const float width_mm = float(HEADER_COLUMNS) * HEADER_CELL_MM;
    const float height_mm = float(HEADER_ROWS) * HEADER_CELL_MM;
    const float origin_x = 0.5f * (chart.settings.plaque_width_mm - width_mm);
    const float origin_y = chart.settings.plaque_height_mm - HEADER_TOP_INSET_MM - height_mm;
    size_t mismatches = 0;
    for (size_t bit_index = 0; bit_index < HEADER_COLUMNS * HEADER_ROWS; ++bit_index) {
        const size_t row = bit_index / HEADER_COLUMNS;
        const size_t column = bit_index % HEADER_COLUMNS;
        const float x_mm = origin_x + (float(column) + 0.5f) * HEADER_CELL_MM;
        const float y_mm = origin_y + (float(HEADER_ROWS - row - 1) + 0.5f) * HEADER_CELL_MM;
        const cv::Point2f point = chart_point_top_down(x_mm, y_mm, chart.settings);
        const cv::Vec4b pixel = warped.at<cv::Vec4b>(std::clamp(int(std::lround(point.y)), 0, warped.rows - 1),
                                                     std::clamp(int(std::lround(point.x)), 0, warped.cols - 1));
        auto distance = [&](const RGBA& color) {
            double value = 0.0;
            for (size_t channel = 0; channel < 3; ++channel) {
                const double delta = double(pixel[channel]) / 255.0 - double(color[channel]);
                value += delta * delta;
            }
            return value;
        };
        const bool decoded = distance(chart.marker_dark) < distance(chart.marker_light);
        const bool wanted = (expected[bit_index / 8] & uint8_t(0x80u >> (bit_index % 8))) != 0;
        mismatches += decoded != wanted;
    }
    return mismatches <= 8;
}

} // namespace

bool SimplePmCalibrationChart::valid() const
{
    return signature != 0 && total_units > 0 && columns > 0 && rows > 0 && capacity > 0 && !patches.empty() && texture.valid();
}

uint64_t simple_pm_calibration_signature(const std::vector<ContinuousColorComponent>& components, ColorMixModel color_mix_model)
{
    uint64_t hash = 1469598103934665603ull;
    const uint8_t schema = uint8_t(SIMPLE_PM_CALIBRATION_SCHEMA_VERSION);
    const uint8_t model = uint8_t(color_mix_model);
    hash_bytes(hash, &schema, sizeof(schema));
    hash_bytes(hash, &model, sizeof(model));
    const uint64_t count = uint64_t(components.size());
    hash_bytes(hash, &count, sizeof(count));
    for (const ContinuousColorComponent& component : components) {
        hash_string(hash, component.color_hex);
        const double td = component.transmission_distance_mm.value_or(-1.0);
        hash_bytes(hash, &td, sizeof(td));
        hash_string(hash, component.material_id.value_or(std::string()));
    }
    return hash;
}

SimplePmCalibrationChart make_simple_pm_calibration_chart(const std::vector<ContinuousColorComponent>& components,
                                                           ColorMixModel color_mix_model,
                                                           const SimplePmCalibrationChartSettings& settings)
{
    SimplePmCalibrationChart chart;
    chart.settings = settings;
    chart.signature = simple_pm_calibration_signature(components, color_mix_model);
    chart.total_units = continuous_color_solver_total_units(components.size());
    if (components.size() < 2 || components.size() > 4 || settings.texture_width < 128 || settings.texture_height < 128 ||
        settings.plaque_width_mm <= 0.f || settings.plaque_height_mm <= 0.f || settings.cell_width_mm <= 0.f ||
        settings.cell_height_mm <= 0.f)
        return chart;

    const float pitch_x = settings.cell_width_mm + std::max(0.f, settings.horizontal_gutter_mm);
    const float pitch_y = settings.cell_height_mm + std::max(0.f, settings.vertical_gutter_mm);
    const float usable_width = settings.plaque_width_mm - 2.f * settings.side_margin_mm;
    const float usable_height = settings.plaque_height_mm - settings.bottom_margin_mm - settings.top_margin_mm;
    chart.columns = usable_width > settings.cell_width_mm ?
                        size_t(std::floor((usable_width + settings.horizontal_gutter_mm) / pitch_x)) :
                        0;
    chart.rows = usable_height > settings.cell_height_mm ?
                     size_t(std::floor((usable_height + settings.vertical_gutter_mm) / pitch_y)) :
                     0;
    chart.capacity = chart.columns * chart.rows;
    if (chart.capacity == 0)
        return chart;

    ContinuousColorSolver printable_solver(components, color_mix_model, true);
    ContinuousColorSolver raw_solver(components, color_mix_model, false);
    if (!printable_solver.valid() || !raw_solver.valid())
        return chart;
    std::vector<RecipeCandidate> recipes = printable_recipes(printable_solver, raw_solver);
    if (recipes.empty())
        return chart;
    chart.total_recipe_count = recipes.size();

    const auto darkest = std::min_element(recipes.begin(), recipes.end(), [](const RecipeCandidate& lhs, const RecipeCandidate& rhs) {
        return luminance(lhs.predicted) < luminance(rhs.predicted);
    });
    const auto lightest = std::max_element(recipes.begin(), recipes.end(), [](const RecipeCandidate& lhs, const RecipeCandidate& rhs) {
        return luminance(lhs.predicted) < luminance(rhs.predicted);
    });
    chart.marker_dark = darkest->target;
    chart.marker_light = lightest->target;

    const double ideal = 1.0 / double(components.size());
    const auto balanced = std::min_element(recipes.begin(), recipes.end(), [&](const RecipeCandidate& lhs, const RecipeCandidate& rhs) {
        auto error = [&](const RecipeCandidate& recipe) {
            double value = 0.0;
            for (uint8_t unit : recipe.units) {
                const double delta = double(unit) / double(chart.total_units) - ideal;
                value += delta * delta;
            }
            return value;
        };
        return error(lhs) < error(rhs);
    });
    chart.guard_color = balanced->target;

    chart.texture.stable_id = "simple-pm-calibration-" + std::to_string(chart.signature);
    chart.texture.display_name = "FullSpectrum Simple PM calibration";
    chart.texture.width = settings.texture_width;
    chart.texture.height = settings.texture_height;
    chart.texture.rgba.assign(size_t(settings.texture_width) * size_t(settings.texture_height) * 4, uint8_t(255));
    RasterPainter painter{chart.texture};
    painter.fill_pixels(0, 0, int(settings.texture_width), int(settings.texture_height), chart.guard_color);

    const float finder_x = 0.5f * FINDER_SIZE_MM + 1.f;
    const float finder_y = 0.5f * FINDER_SIZE_MM + 1.f;
    draw_finder(painter, finder_x, finder_y, settings.plaque_width_mm, settings.plaque_height_mm,
                chart.marker_dark, chart.marker_light);
    draw_finder(painter, settings.plaque_width_mm - finder_x, finder_y, settings.plaque_width_mm,
                settings.plaque_height_mm, chart.marker_dark, chart.marker_light);
    draw_finder(painter, finder_x, settings.plaque_height_mm - finder_y, settings.plaque_width_mm,
                settings.plaque_height_mm, chart.marker_dark, chart.marker_light);
    draw_finder(painter, settings.plaque_width_mm - finder_x, settings.plaque_height_mm - finder_y,
                settings.plaque_width_mm, settings.plaque_height_mm, chart.marker_dark, chart.marker_light);
    draw_header(painter, header_bytes(chart.signature, components.size(), chart.total_units), settings,
                chart.marker_dark, chart.marker_light);

    const size_t selection_capacity = settings.maximum_patch_count == 0 ?
                                          chart.capacity :
                                          std::min(chart.capacity, settings.maximum_patch_count);
    const std::vector<size_t> selected = choose_recipe_indices(recipes, selection_capacity, chart.total_units);
    chart.patches.reserve(selected.size());
    for (size_t patch_index = 0; patch_index < selected.size(); ++patch_index) {
        const size_t row = patch_index / chart.columns;
        const size_t column_in_row = patch_index % chart.columns;
        const size_t column = (row % 2 == 0) ? column_in_row : chart.columns - column_in_row - 1;
        const float x0 = settings.side_margin_mm + float(column) * pitch_x;
        const float y0 = settings.bottom_margin_mm + float(row) * pitch_y;
        const float x1 = x0 + settings.cell_width_mm;
        const float y1 = y0 + settings.cell_height_mm;
        const RecipeCandidate& recipe = recipes[selected[patch_index]];
        painter.fill_mm(x0, y0, x1, y1, settings.plaque_width_mm, settings.plaque_height_mm, recipe.target);

        SimplePmCalibrationPatch patch;
        patch.patch_id = uint32_t(patch_index);
        patch.component_units = recipe.units;
        patch.target_color = recipe.target;
        patch.predicted_color = recipe.predicted;
        patch.uv_rect = {x0 / settings.plaque_width_mm, y0 / settings.plaque_height_mm,
                         x1 / settings.plaque_width_mm, y1 / settings.plaque_height_mm};
        patch.solid_anchor = std::count(recipe.units.begin(), recipe.units.end(), uint8_t(chart.total_units)) == 1;
        chart.patches.emplace_back(std::move(patch));
    }
    return chart;
}

SimplePmPhotoAnalysis analyze_simple_pm_calibration_photo(const std::vector<uint8_t>& photo_rgba,
                                                           uint32_t photo_width,
                                                           uint32_t photo_height,
                                                           const std::vector<ContinuousColorComponent>& components,
                                                           ColorMixModel color_mix_model,
                                                           const SimplePmCalibrationChartSettings& settings)
{
    SimplePmPhotoAnalysis result;
    if (photo_width == 0 || photo_height == 0 || photo_rgba.size() != size_t(photo_width) * size_t(photo_height) * 4) {
        result.error = "The calibration photo is incomplete.";
        return result;
    }
    const SimplePmCalibrationChart chart = make_simple_pm_calibration_chart(components, color_mix_model, settings);
    if (!chart.valid()) {
        result.error = "The current filament set cannot generate the expected calibration chart.";
        return result;
    }

    const cv::Mat source = top_down_rgba(photo_rgba, photo_width, photo_height);
    float finder_confidence = 0.f;
    const std::vector<cv::Point2f> source_points = detect_finders(source, finder_confidence);
    if (source_points.size() != 4) {
        result.error = "Four calibration fiducials could not be found. Photograph the complete rectangle straight-on with even lighting.";
        return result;
    }

    const float finder_x = 0.5f * FINDER_SIZE_MM + 1.f;
    const float finder_y = 0.5f * FINDER_SIZE_MM + 1.f;
    const std::vector<cv::Point2f> destination_points = {
        chart_point_top_down(finder_x, settings.plaque_height_mm - finder_y, settings),
        chart_point_top_down(settings.plaque_width_mm - finder_x, settings.plaque_height_mm - finder_y, settings),
        chart_point_top_down(settings.plaque_width_mm - finder_x, finder_y, settings),
        chart_point_top_down(finder_x, finder_y, settings)};
    const cv::Mat transform = cv::getPerspectiveTransform(source_points, destination_points);
    cv::Mat warped;
    cv::warpPerspective(source, warped, transform, cv::Size(int(settings.texture_width), int(settings.texture_height)),
                        cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    if (warped.empty()) {
        result.error = "The calibration photo could not be rectified.";
        return result;
    }
    if (!header_matches(warped, chart)) {
        result.error =
            "The calibration code does not match the currently selected filaments. Regenerate the rectangle for this material set.";
        return result;
    }

    struct RawSample
    {
        std::array<uint8_t, 3> rgb{};
        float                  confidence{0.f};
    };
    std::vector<RawSample> samples(chart.patches.size());
    std::vector<std::pair<RGBA, std::array<uint8_t, 3>>> anchors;
    for (size_t patch_index = 0; patch_index < chart.patches.size(); ++patch_index) {
        samples[patch_index].rgb = median_patch_rgb(warped, chart.patches[patch_index].uv_rect, samples[patch_index].confidence);
        if (chart.patches[patch_index].solid_anchor && samples[patch_index].confidence >= 0.2f)
            anchors.emplace_back(chart.patches[patch_index].predicted_color, samples[patch_index].rgb);
    }
    const auto camera_transform = fit_camera_channels(anchors);

    result.profile.schema_version = SIMPLE_PM_CALIBRATION_SCHEMA_VERSION;
    result.profile.signature = chart.signature;
    result.profile.color_mix_model = color_mix_model;
    result.profile.total_units = chart.total_units;
    result.profile.components = components;
    result.profile.observations.reserve(chart.patches.size());
    for (size_t patch_index = 0; patch_index < chart.patches.size(); ++patch_index) {
        const float confidence = samples[patch_index].confidence * finder_confidence;
        if (confidence < 0.18f) {
            ++result.rejected_patch_count;
            continue;
        }
        SimplePmCalibrationObservation observation;
        observation.component_units = chart.patches[patch_index].component_units;
        observation.expected_color = chart.patches[patch_index].predicted_color;
        observation.measured_color = normalized_photo_color(samples[patch_index].rgb, camera_transform);
        observation.confidence = confidence;
        result.profile.observations.emplace_back(std::move(observation));
        ++result.accepted_patch_count;
    }
    result.registration_confidence = finder_confidence;
    if (result.accepted_patch_count < std::max<size_t>(components.size() * 4, chart.patches.size() / 5)) {
        result.error = "Too few clean recipe cells were visible. Avoid glare and keep the complete rectangle in frame.";
        return result;
    }
    result.success = true;
    return result;
}

bool save_simple_pm_calibration_profile(const SimplePmCalibrationProfile& profile, std::string* saved_path, std::string* error)
{
    if (profile.schema_version != SIMPLE_PM_CALIBRATION_SCHEMA_VERSION || profile.signature == 0 || profile.components.size() < 2 ||
        profile.observations.size() < profile.components.size()) {
        if (error)
            *error = "The calibration profile is incomplete.";
        return false;
    }
    const std::string path = profile_path(profile.signature);
    boost::system::error_code ec;
    boost::filesystem::create_directories(boost::filesystem::path(path).parent_path(), ec);
    if (ec) {
        if (error)
            *error = ec.message();
        return false;
    }

    nlohmann::json root;
    root["schema_version"] = profile.schema_version;
    root["signature"] = profile.signature;
    root["color_mix_model"] = int(profile.color_mix_model);
    root["total_units"] = profile.total_units;
    root["components"] = nlohmann::json::array();
    for (const ContinuousColorComponent& component : profile.components) {
        nlohmann::json item{{"color_hex", component.color_hex}};
        if (component.transmission_distance_mm)
            item["td_mm"] = *component.transmission_distance_mm;
        if (component.material_id)
            item["material_id"] = *component.material_id;
        root["components"].push_back(std::move(item));
    }
    root["observations"] = nlohmann::json::array();
    for (const SimplePmCalibrationObservation& observation : profile.observations) {
        root["observations"].push_back(nlohmann::json{{"units", observation.component_units},
                                                       {"expected", color_json(observation.expected_color)},
                                                       {"measured", color_json(observation.measured_color)},
                                                       {"confidence", observation.confidence}});
    }
    boost::nowide::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error)
            *error = "The calibration profile file could not be opened.";
        return false;
    }
    output << root.dump(2);
    output.close();
    if (!output.good()) {
        if (error)
            *error = "The calibration profile file could not be written.";
        return false;
    }
    if (saved_path)
        *saved_path = path;
    reload_simple_pm_calibration_profiles();
    return true;
}

void reload_simple_pm_calibration_profiles()
{
    std::lock_guard<std::mutex> lock(g_profile_mutex);
    g_profile_cache.clear();
    g_profile_revision.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t simple_pm_calibration_revision()
{
    return g_profile_revision.load(std::memory_order_acquire);
}

RGBA apply_simple_pm_calibration(const std::vector<ContinuousColorComponent>& components,
                                 ColorMixModel color_mix_model,
                                 const std::vector<double>& normalized_weights,
                                 const RGBA& predicted_color)
{
    if (components.size() < 2 || normalized_weights.size() != components.size())
        return predicted_color;
    const uint64_t signature = simple_pm_calibration_signature(components, color_mix_model);
    const std::optional<SimplePmCalibrationProfile> profile = matching_profile(signature);
    if (!profile || profile->observations.empty() || profile->total_units <= 0)
        return predicted_color;

    double sum = 0.0;
    for (double weight : normalized_weights)
        sum += std::max(0.0, weight);
    if (sum <= std::numeric_limits<double>::epsilon())
        return predicted_color;

    struct Neighbor
    {
        double distance_squared{std::numeric_limits<double>::infinity()};
        const SimplePmCalibrationObservation* observation{nullptr};
    };
    std::array<Neighbor, NEAREST_PROFILE_SAMPLES> nearest;
    for (const SimplePmCalibrationObservation& observation : profile->observations) {
        if (observation.component_units.size() != normalized_weights.size() || observation.confidence <= 0.f)
            continue;
        double distance = 0.0;
        for (size_t component_index = 0; component_index < normalized_weights.size(); ++component_index) {
            const double lhs = std::max(0.0, normalized_weights[component_index]) / sum;
            const double rhs = double(observation.component_units[component_index]) / double(profile->total_units);
            const double delta = lhs - rhs;
            distance += delta * delta;
        }
        if (distance >= nearest.back().distance_squared)
            continue;
        nearest.back() = Neighbor{distance, &observation};
        std::sort(nearest.begin(), nearest.end(), [](const Neighbor& lhs, const Neighbor& rhs) {
            return lhs.distance_squared < rhs.distance_squared;
        });
    }
    if (nearest.front().observation == nullptr || nearest.front().distance_squared > 0.16)
        return predicted_color;

    std::array<double, 3> residual{0.0, 0.0, 0.0};
    double total_weight = 0.0;
    for (const Neighbor& neighbor : nearest) {
        if (neighbor.observation == nullptr)
            continue;
        const double spatial = 1.0 / std::max(1e-6, neighbor.distance_squared);
        const double weight = spatial * double(neighbor.observation->confidence);
        for (size_t channel = 0; channel < 3; ++channel)
            residual[channel] += weight * double(neighbor.observation->measured_color[channel] -
                                                 neighbor.observation->expected_color[channel]);
        total_weight += weight;
    }
    if (total_weight <= 0.0)
        return predicted_color;

    const double distance = std::sqrt(nearest.front().distance_squared);
    const double spatial_fade = std::clamp(1.0 - distance / 0.40, 0.0, 1.0);
    RGBA corrected = predicted_color;
    for (size_t channel = 0; channel < 3; ++channel) {
        const double delta = std::clamp(residual[channel] / total_weight, -0.30, 0.30) * spatial_fade;
        corrected[channel] = clamp01(float(double(predicted_color[channel]) + delta));
    }
    return corrected;
}

} // namespace Slic3r::ImageMap
