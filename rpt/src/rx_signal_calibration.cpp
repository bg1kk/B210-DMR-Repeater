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

const std::vector<int> kStrongInputs{0, -10, -20, -30, -40, -50, -60};
const std::vector<int> kWeakInputs{-60, -70, -80, -90, -100, -110, -120, -121};

const RxSignalCalibrationCurve* curve_for(const RxSignalCalibrationConfig& config,
                                          int rx_channel,
                                          RxCalibrationBand band)
{
    if (rx_channel < 0 || rx_channel >= 2) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(rx_channel);
    return band == RxCalibrationBand::Strong ? &config.strong[index]
                                              : &config.weak[index];
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
    return band == RxCalibrationBand::Strong ? "strong" : "weak";
}

std::optional<RxCalibrationBand> rx_calibration_band_from_string(
    const std::string& value)
{
    if (value == "strong") return RxCalibrationBand::Strong;
    if (value == "weak") return RxCalibrationBand::Weak;
    return std::nullopt;
}

const std::vector<int>& rx_calibration_required_inputs(RxCalibrationBand band)
{
    return band == RxCalibrationBand::Strong ? kStrongInputs : kWeakInputs;
}

bool rx_calibration_curve_complete(const RxSignalCalibrationCurve& curve,
                                   RxCalibrationBand band)
{
    if (!curve.rx_gain_tenths_db || curve.points.size() !=
        rx_calibration_required_inputs(band).size()) {
        return false;
    }
    std::vector<RxSignalCalibrationPoint> points = curve.points;
    std::sort(points.begin(), points.end(),
              [](const auto& left, const auto& right) {
                  return left.input_dbm > right.input_dbm;
              });
    std::set<int> inputs;
    double previous_dbfs = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        if (!std::isfinite(point.measured_dbfs) || !std::isfinite(point.snr_db) ||
            point.snr_db < 12.0 ||
            point.input_dbm != rx_calibration_required_inputs(band)[index] ||
            !inputs.insert(point.input_dbm).second ||
            point.measured_dbfs >= previous_dbfs) {
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
    for (const RxCalibrationBand band : {RxCalibrationBand::Strong,
                                         RxCalibrationBand::Weak}) {
        const RxSignalCalibrationCurve* curve = curve_for(config, rx_channel, band);
        if (!curve || !curve->rx_gain_tenths_db ||
            *curve->rx_gain_tenths_db != rx_gain_tenths_db ||
            !rx_calibration_curve_complete(*curve, band)) {
            continue;
        }
        if (const auto dbm = interpolate(*curve, *rssi_dbfs)) {
            result.rssi_dbm = *dbm;
            result.calibrated = true;
            return result;
        }
    }
    return result;
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
    std::optional<double> snr_db, std::int64_t observed_at_ms)
{
    if (rx_channel < 0 || rx_channel >= 2) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    observations_[static_cast<std::size_t>(rx_channel)] =
        RxCalibrationObservation{rx_gain_tenths_db, measured_dbfs, noise_dbfs,
                                 snr_db, observed_at_ms};
}

RxCalibrationReading RxSignalCalibrationRuntime::reading(
    int rx_channel, std::int32_t rx_gain_tenths_db,
    std::optional<double> measured_dbfs) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rx_calibration_reading(config_, rx_channel, rx_gain_tenths_db,
                                  measured_dbfs);
}

std::optional<RxCalibrationObservation>
RxSignalCalibrationRuntime::observation(int rx_channel) const
{
    if (rx_channel < 0 || rx_channel >= 2) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return observations_[static_cast<std::size_t>(rx_channel)];
}

} // namespace dmr_rpt
