#pragma once
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <ctime>
#include <functional>
#include <limits>

#include "opencv2/opencv.hpp"

class QuantKMeans
{
public:
    using ProgressFn = std::function<bool(int current, int total)>;
    static constexpr int MAX_TRAINING_SAMPLES = 32'768;

    int     m_alpha_thres;
    cv::Mat m_flatten_labels;
    cv::Mat m_centers8UC3;
    QuantKMeans(int alpha_thres = 10) : m_alpha_thres(alpha_thres) {}
    void apply(cv::Mat &ori_image, cv::Mat &new_image, int num_cluster, int color_space)
    {
        cv::Mat image;
        convert_color_space(ori_image, image, color_space);
        cv::Mat flatten_image = flatten(image);

        apply(flatten_image, num_cluster, color_space);
        replace_centers(ori_image, new_image);
    }
    void apply_aplha(cv::Mat &ori_image, cv::Mat &new_image, int num_cluster, int color_space)
    {
        // cout << " *** DoAlpha *** " << endl;
        cv::Mat flatten_image8UC3 = flatten_alpha(ori_image);
        cv::Mat image8UC3;
        convert_color_space(flatten_image8UC3, image8UC3, color_space);
        cv::Mat image32FC3(image8UC3.rows, 1, CV_32FC3);
        for (int i = 0; i < image8UC3.rows; i++)
            image32FC3.at<cv::Vec3f>(i, 0) = image8UC3.at<cv::Vec3b>(i, 0);

        apply(image32FC3, num_cluster, color_space);
        repalce_centers_aplha(ori_image, new_image);
    }
    void apply(cv::Mat &flatten_image, int num_cluster, int color_space)
    {
        cv::Mat centers32FC3;
        num_cluster = fmin(flatten_image.rows, num_cluster);
        kmeans(flatten_image, num_cluster, this->m_flatten_labels, cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 300, 0.5), 3, cv::KMEANS_PP_CENTERS,
               centers32FC3);
        this->m_centers8UC3 = cv::Mat(num_cluster, 1, CV_8UC3);
        for (int i = 0; i < num_cluster; i++) this->m_centers8UC3.at<cv::Vec3b>(i) = centers32FC3.at<cv::Vec3f>(i);

        convert_color_space(this->m_centers8UC3, this->m_centers8UC3, color_space, true);
    }
    bool apply(const std::vector<std::array<float, 4>> &ori_colors,
               std::vector<std::array<float, 4>> &      cluster_results,
               std::vector<int> &                       labels,
               int                                      num_cluster = -1,
               int                                      max_cluster = 15,
               int                                      color_space = 2,
               const ProgressFn&                        progress_fn = {})
    {
        if (progress_fn && !progress_fn(0, 100))
            return false;
        // Training OpenCV k-means on every image-map sample makes its cost grow
        // with the tessellated preview rather than the source image. Use a
        // bounded deterministic training set, then classify every original
        // sample against the final centers in one linear pass.
        cv::Mat flatten_image8UC3 = flatten_vector(ori_colors);
        cv::Mat training_image8UC3 = bounded_training_sample(flatten_image8UC3, MAX_TRAINING_SAMPLES);

        std::vector<int> training_labels;
        const ProgressFn training_progress = progress_fn ?
            ProgressFn([&progress_fn](int current, int total) {
                const int safe_total = std::max(total, 1);
                return progress_fn(std::clamp(80 * current / safe_total, 0, 80), 100);
            }) :
            ProgressFn{};
        if (!this->apply(training_image8UC3,
                         cluster_results,
                         training_labels,
                         num_cluster,
                         max_cluster,
                         color_space,
                         training_progress))
            return false;

        if (training_image8UC3.rows == flatten_image8UC3.rows) {
            labels = std::move(training_labels);
            return !progress_fn || progress_fn(100, 100);
        }
        return assign_to_centers(flatten_image8UC3, labels, color_space, progress_fn, 80, 100);
    }
    bool apply(const cv::Mat &                    flatten_image8UC3,
               std::vector<std::array<float, 4>> &cluster_results,
               std::vector<int> &                 labels,
               int                                num_cluster = -1,
               int                                max_cluster = 15,
               int                                color_space = 2,
               const ProgressFn&                  progress_fn = {})
    {
        const bool automatic_cluster_count = num_cluster < 1;
        const int  progress_total = 5;
        int        progress_current = 0;
        auto report_progress = [&](bool finished = false) {
            if (!progress_fn)
                return true;
            const int current = finished ? progress_total : std::min(progress_current, progress_total - 1);
            return progress_fn(current, progress_total);
        };
        if (!report_progress())
            return false;

        cv::Mat image8UC3;
        convert_color_space(flatten_image8UC3, image8UC3, color_space);
        if (image8UC3.empty())
            return false;
        max_cluster = std::clamp(max_cluster, 1, image8UC3.rows);

        cv::Mat image32FC3(image8UC3.rows, 1, CV_32FC3);
        for (int i = 0; i < image8UC3.rows; i++)
            image32FC3.at<cv::Vec3f>(i, 0) = image8UC3.at<cv::Vec3b>(i, 0);
        ++progress_current;
        if (!report_progress())
            return false;

        int best_cluster = 1;
        if (!automatic_cluster_count)
            num_cluster = std::clamp(num_cluster, 1, max_cluster);
        if (automatic_cluster_count) {
            // Raw k-means compactness is monotonically improved by adding
            // clusters, so the previous search over 1..max_cluster invariably
            // selected the largest non-duplicate count after many complete
            // k-means runs. Determine that count directly and run k-means once.
            best_cluster = this->more_than_request(image8UC3, max_cluster) ?
                               max_cluster :
                               compute_num_colors(image8UC3);
            best_cluster = std::max(best_cluster, 1);
            ++progress_current;
            if (!report_progress())
                return false;
        } else if (this->more_than_request(image8UC3, num_cluster)) {
            best_cluster = num_cluster;
            ++progress_current;
            if (!report_progress())
                return false;
        } else {
            best_cluster = compute_num_colors(image8UC3);
            std::cout << "num of image color is " << best_cluster << ", less than custom number " << num_cluster << std::endl;
            ++progress_current;
            if (!report_progress())
                return false;
        }

        cv::Mat centers32FC3;
        cv::kmeans(image32FC3, best_cluster, this->m_flatten_labels, cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 300, 0.5), 3, cv::KMEANS_PP_CENTERS,
                   centers32FC3);
        ++progress_current;
        if (!report_progress())
            return false;
        this->m_centers8UC3 = cv::Mat(best_cluster, 1, CV_8UC3);
        for (int i = 0; i < best_cluster; i++) {
            auto center                          = centers32FC3.row(i);
            this->m_centers8UC3.at<cv::Vec3b>(i) = {uchar(center.at<float>(0)), uchar(center.at<float>(1)), uchar(center.at<float>(2))};
        }
        convert_color_space(this->m_centers8UC3, this->m_centers8UC3, color_space, true);

        cluster_results.clear();
        labels.clear();
        for (int i = 0; i < flatten_image8UC3.rows; i++)
            labels.emplace_back(this->m_flatten_labels.at<int>(i, 0));
        for (int i = 0; i < best_cluster; i++) {
            cv::Vec3f center = this->m_centers8UC3.at<cv::Vec3b>(i, 0);
            cluster_results.emplace_back(std::array<float, 4>{center[0] / 255.f, center[1] / 255.f, center[2] / 255.f, 1.f});
        }
        return report_progress(true);
    }

    cv::Mat bounded_training_sample(const cv::Mat &image8UC3, int max_samples)
    {
        if (image8UC3.rows <= max_samples)
            return image8UC3;

        const int distribution_samples = std::max(1, max_samples * 3 / 4);
        const int gamut_budget         = std::max(0, max_samples - distribution_samples);
        cv::Mat   sample(max_samples, 1, CV_8UC3);

        // Preserve the source distribution with evenly spaced samples.
        for (int sample_idx = 0; sample_idx < distribution_samples; ++sample_idx) {
            const uint64_t numerator = (uint64_t(2 * sample_idx) + 1u) * uint64_t(image8UC3.rows);
            const int source_idx = std::min(image8UC3.rows - 1, int(numerator / uint64_t(2 * distribution_samples)));
            sample.at<cv::Vec3b>(sample_idx, 0) = image8UC3.at<cv::Vec3b>(source_idx, 0);
        }

        // Reserve part of the training set for gamut coverage so small but
        // visually important color regions are not lost by spatial sampling.
        constexpr int bucket_count = 32 * 32 * 32;
        std::vector<int> bucket_sources(bucket_count, -1);
        for (int source_idx = 0; source_idx < image8UC3.rows; ++source_idx) {
            const cv::Vec3b color = image8UC3.at<cv::Vec3b>(source_idx, 0);
            const int bucket = (int(color[0]) >> 3) * 32 * 32 + (int(color[1]) >> 3) * 32 + (int(color[2]) >> 3);
            if (bucket_sources[bucket] < 0)
                bucket_sources[bucket] = source_idx;
        }
        std::vector<int> occupied_sources;
        occupied_sources.reserve(bucket_count);
        for (int source_idx : bucket_sources) {
            if (source_idx >= 0)
                occupied_sources.emplace_back(source_idx);
        }

        const int gamut_samples = std::min(gamut_budget, int(occupied_sources.size()));
        for (int sample_idx = 0; sample_idx < gamut_samples; ++sample_idx) {
            const uint64_t numerator = (uint64_t(2 * sample_idx) + 1u) * uint64_t(occupied_sources.size());
            const size_t occupied_idx = std::min(occupied_sources.size() - 1,
                                                 size_t(numerator / uint64_t(2 * gamut_samples)));
            sample.at<cv::Vec3b>(distribution_samples + sample_idx, 0) =
                image8UC3.at<cv::Vec3b>(occupied_sources[occupied_idx], 0);
        }

        const int populated = distribution_samples + gamut_samples;
        return populated == max_samples ? sample : sample.rowRange(0, populated).clone();
    }

    bool assign_to_centers(const cv::Mat &image8UC3,
                           std::vector<int> &labels,
                           int color_space,
                           const ProgressFn &progress_fn,
                           int progress_begin,
                           int progress_end)
    {
        if (image8UC3.empty() || m_centers8UC3.empty())
            return false;

        cv::Mat converted_image;
        cv::Mat converted_centers;
        convert_color_space(image8UC3, converted_image, color_space);
        convert_color_space(m_centers8UC3, converted_centers, color_space);

        labels.resize(size_t(converted_image.rows));
        const int progress_span = std::max(0, progress_end - progress_begin);
        const int progress_stride = std::max(converted_image.rows / 100, 1);
        for (int row = 0; row < converted_image.rows; ++row) {
            if (progress_fn && row % progress_stride == 0 &&
                !progress_fn(progress_begin + progress_span * row / converted_image.rows, 100))
                return false;

            const cv::Vec3b color = converted_image.at<cv::Vec3b>(row, 0);
            int best_center = 0;
            int best_distance = std::numeric_limits<int>::max();
            for (int center_idx = 0; center_idx < converted_centers.rows; ++center_idx) {
                const cv::Vec3b center = converted_centers.at<cv::Vec3b>(center_idx, 0);
                const int d0 = int(color[0]) - int(center[0]);
                const int d1 = int(color[1]) - int(center[1]);
                const int d2 = int(color[2]) - int(center[2]);
                const int distance = d0 * d0 + d1 * d1 + d2 * d2;
                if (distance < best_distance) {
                    best_distance = distance;
                    best_center   = center_idx;
                }
            }
            labels[size_t(row)] = best_center;
        }
        return !progress_fn || progress_fn(progress_end, 100);
    }

    bool more_than_request(const cv::Mat &image8UC3, int target_num)
    {
        std::vector<cv::Vec3b> uniqueImage;
        cv::Vec3b              cur_color;
        for (int i = 0; i < image8UC3.rows; i++) {
            cur_color = image8UC3.at<cv::Vec3b>(i, 0);
            if (!is_in(cur_color, uniqueImage)) {
                uniqueImage.emplace_back(cur_color);
                if (uniqueImage.size() >= target_num) return true;
            }
        }
        return false;
    }

    int compute_num_colors(const cv::Mat &image8UC3)
    {
        std::vector<cv::Vec3b> uniqueImage;
        cv::Vec3b              cur_color;
        for (int i = 0; i < image8UC3.rows; i++) {
            cur_color = image8UC3.at<cv::Vec3b>(i, 0);
            if (!is_in(cur_color, uniqueImage)) uniqueImage.emplace_back(cur_color);
        }

        return uniqueImage.size();
    }

    bool is_in(const cv::Vec3b &cur_color, const std::vector<cv::Vec3b> &uniqueImage)
    {
        for (auto &color : uniqueImage)
            if (cur_color[0] == color[0] && cur_color[1] == color[1] && cur_color[2] == color[2]) return true;
        return false;
    }

    bool repeat_center(int cur_cluster, const cv::Mat &centers32FC3, int color_space)
    {
        cv::Mat centers8UC3 = cv::Mat(cur_cluster, 1, CV_8UC3);
        for (int i = 0; i < cur_cluster; i++) {
            auto center = centers32FC3.row(i);
            centers8UC3.at<cv::Vec3b>(i) = {uchar(center.at<float>(0)), uchar(center.at<float>(1)), uchar(center.at<float>(2))};
        }
        convert_color_space(centers8UC3, centers8UC3, color_space, true);
        std::vector<cv::Vec3b> unique_centers;
        cv::Vec3b              cur_center;
        for (int i = 0; i < cur_cluster; i++) {
            cur_center = centers8UC3.at<cv::Vec3b>(i, 0);
            if (!is_in(cur_center, unique_centers))
                unique_centers.emplace_back(cur_center);
            else
                return true;
        }
        return false;
    }

    void replace_centers(cv::Mat &ori_image, cv::Mat &new_image)
    {
        for (int i = 0; i < ori_image.rows; i++) {
            for (int j = 0; j < ori_image.cols; j++) {
                int       idx                 = this->m_flatten_labels.at<int>(i * ori_image.cols + j, 0);
                cv::Vec3b pixel               = this->m_centers8UC3.at<cv::Vec3b>(idx);
                new_image.at<cv::Vec3b>(i, j) = pixel;
            }
        }
    }
    void repalce_centers_aplha(cv::Mat &ori_image, cv::Mat &new_image)
    {
        int       cnt = 0;
        int       idx;
        cv::Vec3b center;
        for (int i = 0; i < ori_image.rows; i++) {
            for (int j = 0; j < ori_image.cols; j++) {
                cv::Vec4b pixel = ori_image.at<cv::Vec4b>(i, j);
                if ((int) pixel[3] < this->m_alpha_thres)
                    new_image.at<cv::Vec4b>(i, j) = pixel;
                else {
                    idx                           = this->m_flatten_labels.at<int>(cnt++, 0);
                    center                        = this->m_centers8UC3.at<cv::Vec3b>(idx);
                    new_image.at<cv::Vec4b>(i, j) = cv::Vec4b(center[0], center[1], center[2], pixel[3]);
                }
            }
        }
    }

    void convert_color_space(const cv::Mat &ori_image, cv::Mat &image, int color_space, bool reverse = false)
    {
        switch (color_space) {
        case 0: image = ori_image; break;
        case 1:
            if (reverse)
                cvtColor(ori_image, image, cv::COLOR_HSV2BGR);
            else
                cvtColor(ori_image, image, cv::COLOR_BGR2HSV);
            break;
        case 2:
            if (reverse)
                cvtColor(ori_image, image, cv::COLOR_Lab2BGR);
            else
                cvtColor(ori_image, image, cv::COLOR_BGR2Lab);
            break;
        default: break;
        }
    }

    cv::Mat flatten(cv::Mat &image)
    {
        int     num_pixels = image.rows * image.cols;
        cv::Mat img(num_pixels, 1, CV_32FC3);
        for (int i = 0; i < image.rows; i++) {
            for (int j = 0; j < image.cols; j++) {
                cv::Vec3f pixel                          = image.at<cv::Vec3b>(i, j);
                img.at<cv::Vec3f>(i * image.cols + j, 0) = pixel;
            }
        }
        return img;
    }
    cv::Mat flatten_alpha(cv::Mat &image)
    {
        int num_pixels = image.rows * image.cols;
        for (int i = 0; i < image.rows; i++)
            for (int j = 0; j < image.cols; j++) {
                cv::Vec4b pixel = image.at<cv::Vec4b>(i, j);
                if ((int) pixel[3] < this->m_alpha_thres) num_pixels--;
            }

        cv::Mat img(num_pixels, 1, CV_8UC3);
        int     cnt = 0;
        for (int i = 0; i < image.rows; i++) {
            for (int j = 0; j < image.cols; j++) {
                cv::Vec4b pixel = image.at<cv::Vec4b>(i, j);
                if ((int) pixel[3] >= this->m_alpha_thres) img.at<cv::Vec3b>(cnt++, 0) = cv::Vec3b(pixel[0], pixel[1], pixel[2]);
            }
        }
        return img;
    }
    cv::Mat flatten_vector(const std::vector<std::array<float, 4>> &ori_colors)
    {
        int num_pixels = ori_colors.size();

        cv::Mat image8UC3(num_pixels, 1, CV_8UC3);
        for (int i = 0; i < num_pixels; i++) {
            std::array<float, 4> pixel    = ori_colors[i];
            image8UC3.at<cv::Vec3b>(i, 0) = cv::Vec3b((int) (pixel[0] * 255.f), (int) (pixel[1] * 255.f), (int) (pixel[2] * 255.f));
        }
        return image8UC3;
    }
};
