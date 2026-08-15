// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

namespace dmr_rpt {

enum class RxCalibrationBand { Low, Medium, High };

struct RxSignalCalibrationPoint {
    int input_dbm = 0;
    double measured_dbfs = 0.0;
    double snr_db = 0.0;
    std::string calibrated_at_utc;
};

struct RxSignalCalibrationSegment {
    double measured_dbfs_low = 0.0;
    double measured_dbfs_high = 0.0;
    double slope_dbm_per_dbfs = 0.0;
    double intercept_dbm = 0.0;
};

struct RxSignalCalibrationCurve {
    std::optional<std::int32_t> rx_gain_tenths_db;
    std::vector<RxSignalCalibrationPoint> points;
    std::vector<RxSignalCalibrationSegment> fit_segments;
};

struct RxSignalCalibrationConfig {
    std::array<RxSignalCalibrationCurve, 2> low;
    std::array<RxSignalCalibrationCurve, 2> medium;
    std::array<RxSignalCalibrationCurve, 2> high;
};

struct RxCalibrationReading {
    std::optional<double> rssi_dbm;
    bool calibrated = false;
    std::optional<std::int32_t> reference_gain_tenths_db;
    std::optional<double> gain_compensation_db;
    std::optional<double> compensated_dbfs;
};

struct RxCalibrationObservation {
    std::int32_t rx_gain_tenths_db = 0;
    std::optional<double> measured_dbfs;
    std::optional<double> noise_dbfs;
    std::optional<double> snr_db;
    std::int64_t observed_at_ms = 0;
    bool receiving = false;
};

class RxSignalCalibrationRuntime {
public:
    explicit RxSignalCalibrationRuntime(RxSignalCalibrationConfig config = {});

    void replace(RxSignalCalibrationConfig config);
    void observe(int rx_channel, std::int32_t rx_gain_tenths_db,
                 std::optional<double> measured_dbfs,
                 std::optional<double> noise_dbfs,
                 std::optional<double> snr_db,
                 std::int64_t observed_at_ms,
                 bool receiving = false);
    RxCalibrationReading reading(int rx_channel, std::int32_t rx_gain_tenths_db,
                                 std::optional<double> measured_dbfs) const;
    std::optional<double> reference_dbfs(
        int rx_channel, RxCalibrationBand band, int input_dbm,
        std::int32_t expected_gain_tenths_db) const;
    std::optional<RxCalibrationObservation> observation(int rx_channel) const;
    std::optional<RxCalibrationObservation> stable_observation(
        int rx_channel, std::size_t required_samples,
        std::int64_t now_ms, std::int64_t maximum_age_ms,
        double maximum_span_dbfs) const;
    void clear_observations(int rx_channel);

private:
    mutable std::mutex mutex_;
    RxSignalCalibrationConfig config_;
    std::array<std::deque<RxCalibrationObservation>, 2> observations_;
};

const char* to_string(RxCalibrationBand band);
std::optional<RxCalibrationBand> rx_calibration_band_from_string(
    const std::string& value);
const std::vector<int>& rx_calibration_required_inputs(RxCalibrationBand band);
bool rx_calibration_curve_complete(const RxSignalCalibrationCurve& curve,
                                   RxCalibrationBand band);
void fit_rx_signal_calibration_curve(RxSignalCalibrationCurve& curve);
RxCalibrationReading rx_calibration_reading(
    const RxSignalCalibrationConfig& config, int rx_channel,
    std::int32_t rx_gain_tenths_db, std::optional<double> rssi_dbfs);
std::optional<double> rx_calibration_reference_dbfs(
    const RxSignalCalibrationConfig& config, int rx_channel,
    RxCalibrationBand band, int input_dbm,
    std::int32_t expected_gain_tenths_db);

} // namespace dmr_rpt
