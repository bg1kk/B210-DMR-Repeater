// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/rx_signal_calibration.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace dmr_rpt {
namespace {

const std::vector<int> kLowInputs{-55, -50, -45, -40, -35, -30, -25, -20};
const std::vector<int> kMediumInputs{-85, -80, -75, -70, -65, -60, -55, -50, -45};
const std::vector<int> kHighInputs{
    -125, -120, -115, -110, -105, -100, -95, -90, -85, -80, -75};

const RxSignalCalibrationCurve* curve_for(const RxSignalCalibrationConfig& config,
                                          int rx_channel,
                                          RxCalibrationBand band)
{
    if (rx_channel < 0 || rx_channel >= 2) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(rx_channel);
    if (band == RxCalibrationBand::Low) return &config.low[index];
    if (band == RxCalibrationBand::Medium) return &config.medium[index];
    return &config.high[index];
}

std::optional<double> interpolate(const RxSignalCalibrationCurve& curve,
                                  double measured_dbfs)
{
    std::vector<RxSignalCalibrationPoint> points = curve.points;
    std::sort(points.begin(), points.end(),
              [](const auto& left, const auto& right) {
                  return left.measured_dbfs < right.measured_dbfs;
              });
    if (points.empty() || measured_dbfs < points.front().measured_dbfs ||
        measured_dbfs > points.back().measured_dbfs) {
        return std::nullopt;
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        const auto& lower = points[index - 1U];
        const auto& upper = points[index];
        if (measured_dbfs > upper.measured_dbfs) {
            continue;
        }
        const double span = upper.measured_dbfs - lower.measured_dbfs;
        if (span <= 0.0) {
            return std::nullopt;
        }
        const double fraction = (measured_dbfs - lower.measured_dbfs) / span;
        return static_cast<double>(lower.input_dbm) + fraction *
            static_cast<double>(upper.input_dbm - lower.input_dbm);
    }
    return static_cast<double>(points.back().input_dbm);
}

} // namespace

const char* to_string(RxCalibrationBand band)
{
    if (band == RxCalibrationBand::Low) return "low";
    if (band == RxCalibrationBand::Medium) return "medium";
    return "high";
}

std::optional<RxCalibrationBand> rx_calibration_band_from_string(
    const std::string& value)
{
    if (value == "low") return RxCalibrationBand::Low;
    if (value == "medium") return RxCalibrationBand::Medium;
    if (value == "high") return RxCalibrationBand::High;
    return std::nullopt;
}

const std::vector<int>& rx_calibration_required_inputs(RxCalibrationBand band)
{
    if (band == RxCalibrationBand::Low) return kLowInputs;
    if (band == RxCalibrationBand::Medium) return kMediumInputs;
    return kHighInputs;
}

bool rx_calibration_curve_complete(const RxSignalCalibrationCurve& curve,
                                   RxCalibrationBand band)
{
    if (!curve.rx_gain_tenths_db || curve.points.size() !=
        rx_calibration_required_inputs(band).size()) {
        return false;
    }
    if ((band == RxCalibrationBand::Low && *curve.rx_gain_tenths_db != 0) ||
        ((band == RxCalibrationBand::Medium || band == RxCalibrationBand::High) &&
         *curve.rx_gain_tenths_db <= 0)) {
        return false;
    }
    std::vector<RxSignalCalibrationPoint> points = curve.points;
    std::sort(points.begin(), points.end(),
              [](const auto& left, const auto& right) {
                  return left.input_dbm < right.input_dbm;
              });
    std::set<int> inputs;
    double previous_dbfs = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        const double minimum_snr_db = band == RxCalibrationBand::High ? 12.0 : 10.0;
        if (!std::isfinite(point.measured_dbfs) || !std::isfinite(point.snr_db) ||
            point.snr_db < minimum_snr_db ||
            point.input_dbm != rx_calibration_required_inputs(band)[index] ||
            !inputs.insert(point.input_dbm).second ||
            point.measured_dbfs <= previous_dbfs) {
            return false;
        }
        previous_dbfs = point.measured_dbfs;
    }
    return true;
}

void fit_rx_signal_calibration_curve(RxSignalCalibrationCurve& curve)
{
    curve.fit_segments.clear();
    std::vector<RxSignalCalibrationPoint> points = curve.points;
    std::sort(points.begin(), points.end(),
              [](const auto& left, const auto& right) {
                  return left.measured_dbfs < right.measured_dbfs;
              });
    for (std::size_t index = 1; index < points.size(); ++index) {
        const auto& low = points[index - 1U];
        const auto& high = points[index];
        const double span = high.measured_dbfs - low.measured_dbfs;
        if (span <= 0.0) continue;
        const double slope = static_cast<double>(high.input_dbm - low.input_dbm) / span;
        curve.fit_segments.push_back({
            low.measured_dbfs, high.measured_dbfs, slope,
            static_cast<double>(low.input_dbm) - slope * low.measured_dbfs});
    }
}

RxCalibrationReading rx_calibration_reading(
    const RxSignalCalibrationConfig& config, int rx_channel,
    std::int32_t rx_gain_tenths_db, std::optional<double> rssi_dbfs)
{
    RxCalibrationReading result;
    if (!rssi_dbfs || !std::isfinite(*rssi_dbfs)) {
        return result;
    }
    std::optional<double> best_gain_distance_db;
    for (const RxCalibrationBand band : {RxCalibrationBand::Low,
                                         RxCalibrationBand::Medium,
                                         RxCalibrationBand::High}) {
        const RxSignalCalibrationCurve* curve = curve_for(config, rx_channel, band);
        if (!curve || !curve->rx_gain_tenths_db ||
            !rx_calibration_curve_complete(*curve, band)) {
            continue;
        }
        const double gain_compensation_db =
            (*curve->rx_gain_tenths_db - rx_gain_tenths_db) / 10.0;
        const double compensated_dbfs = *rssi_dbfs + gain_compensation_db;
        if (const auto dbm = interpolate(*curve, compensated_dbfs)) {
            const double gain_distance_db = std::abs(gain_compensation_db);
            if (!best_gain_distance_db ||
                gain_distance_db < *best_gain_distance_db) {
                best_gain_distance_db = gain_distance_db;
                result.rssi_dbm = *dbm;
                result.calibrated = true;
                result.reference_gain_tenths_db =
                    *curve->rx_gain_tenths_db;
                result.gain_compensation_db = gain_compensation_db;
                result.compensated_dbfs = compensated_dbfs;
            }
        }
    }
    return result;
}

std::optional<double> rx_calibration_reference_dbfs(
    const RxSignalCalibrationConfig& config, int rx_channel,
    RxCalibrationBand band, int input_dbm,
    std::int32_t expected_gain_tenths_db)
{
    const RxSignalCalibrationCurve* curve = curve_for(config, rx_channel, band);
    if (!curve || !curve->rx_gain_tenths_db ||
        *curve->rx_gain_tenths_db != expected_gain_tenths_db ||
        !rx_calibration_curve_complete(*curve, band)) {
        return std::nullopt;
    }
    const auto point = std::find_if(
        curve->points.begin(), curve->points.end(),
        [input_dbm](const RxSignalCalibrationPoint& item) {
            return item.input_dbm == input_dbm;
        });
    return point == curve->points.end()
        ? std::nullopt
        : std::optional<double>(point->measured_dbfs);
}

RxSignalCalibrationRuntime::RxSignalCalibrationRuntime(
    RxSignalCalibrationConfig config)
    : config_(std::move(config))
{
}

void RxSignalCalibrationRuntime::replace(RxSignalCalibrationConfig config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = std::move(config);
}

void RxSignalCalibrationRuntime::observe(
    int rx_channel, std::int32_t rx_gain_tenths_db,
    std::optional<double> measured_dbfs, std::optional<double> noise_dbfs,
    std::optional<double> snr_db, std::int64_t observed_at_ms,
    bool receiving)
{
    if (rx_channel < 0 || rx_channel >= 2) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto& history = observations_[static_cast<std::size_t>(rx_channel)];
    history.push_back({rx_gain_tenths_db, measured_dbfs, noise_dbfs,
                       snr_db, observed_at_ms, receiving});
    while (history.size() > 10U) {
        history.pop_front();
    }
}

RxCalibrationReading RxSignalCalibrationRuntime::reading(
    int rx_channel, std::int32_t rx_gain_tenths_db,
    std::optional<double> measured_dbfs) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rx_calibration_reading(config_, rx_channel, rx_gain_tenths_db,
                                  measured_dbfs);
}

std::optional<double> RxSignalCalibrationRuntime::reference_dbfs(
    int rx_channel, RxCalibrationBand band, int input_dbm,
    std::int32_t expected_gain_tenths_db) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rx_calibration_reference_dbfs(config_, rx_channel, band, input_dbm,
                                         expected_gain_tenths_db);
}

std::optional<RxCalibrationObservation>
RxSignalCalibrationRuntime::observation(int rx_channel) const
{
    if (rx_channel < 0 || rx_channel >= 2) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& history = observations_[static_cast<std::size_t>(rx_channel)];
    if (history.empty()) {
        return std::nullopt;
    }
    return history.back();
}

std::optional<RxCalibrationObservation>
RxSignalCalibrationRuntime::stable_observation(
    int rx_channel, std::size_t required_samples, std::int64_t now_ms,
    std::int64_t maximum_age_ms, double maximum_span_dbfs) const
{
    if (rx_channel < 0 || rx_channel >= 2 || required_samples == 0U ||
        maximum_age_ms < 0 || maximum_span_dbfs < 0.0) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& history = observations_[static_cast<std::size_t>(rx_channel)];
    if (history.size() < required_samples) {
        return std::nullopt;
    }
    double minimum_dbfs = std::numeric_limits<double>::infinity();
    double maximum_dbfs = -std::numeric_limits<double>::infinity();
    const std::size_t first = history.size() - required_samples;
    for (std::size_t index = first; index < history.size(); ++index) {
        const RxCalibrationObservation& observation = history[index];
        if (!observation.measured_dbfs ||
            now_ms < observation.observed_at_ms ||
            now_ms - observation.observed_at_ms > maximum_age_ms) {
            return std::nullopt;
        }
        minimum_dbfs = std::min(minimum_dbfs, *observation.measured_dbfs);
        maximum_dbfs = std::max(maximum_dbfs, *observation.measured_dbfs);
    }
    if (maximum_dbfs - minimum_dbfs > maximum_span_dbfs) {
        return std::nullopt;
    }
    return history.back();
}

void RxSignalCalibrationRuntime::clear_observations(int rx_channel)
{
    if (rx_channel < 0 || rx_channel >= 2) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    observations_[static_cast<std::size_t>(rx_channel)].clear();
}

} // namespace dmr_rpt
