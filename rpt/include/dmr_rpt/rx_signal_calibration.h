// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

namespace dmr_rpt {

enum class RxCalibrationBand { Strong, Weak };

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
    std::array<RxSignalCalibrationCurve, 2> strong;
    std::array<RxSignalCalibrationCurve, 2> weak;
};

struct RxCalibrationReading {
    std::optional<double> rssi_dbm;
    bool calibrated = false;
};

struct RxCalibrationObservation {
    std::int32_t rx_gain_tenths_db = 0;
    std::optional<double> measured_dbfs;
    std::optional<double> noise_dbfs;
    std::optional<double> snr_db;
    std::int64_t observed_at_ms = 0;
};

class RxSignalCalibrationRuntime {
public:
    explicit RxSignalCalibrationRuntime(RxSignalCalibrationConfig config = {});

    void replace(RxSignalCalibrationConfig config);
    void observe(int rx_channel, std::int32_t rx_gain_tenths_db,
                 std::optional<double> measured_dbfs,
                 std::optional<double> noise_dbfs,
                 std::optional<double> snr_db,
                 std::int64_t observed_at_ms);
    RxCalibrationReading reading(int rx_channel, std::int32_t rx_gain_tenths_db,
                                 std::optional<double> measured_dbfs) const;
    std::optional<RxCalibrationObservation> observation(int rx_channel) const;

private:
    mutable std::mutex mutex_;
    RxSignalCalibrationConfig config_;
    std::array<std::optional<RxCalibrationObservation>, 2> observations_;
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

} // namespace dmr_rpt
