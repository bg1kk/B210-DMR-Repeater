// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/rx_signal_calibration.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

dmr_rpt::RxSignalCalibrationCurve make_curve(
    dmr_rpt::RxCalibrationBand band, std::int32_t gain_tenths_db,
    double snr_db = 20.0)
{
    dmr_rpt::RxSignalCalibrationCurve curve;
    curve.rx_gain_tenths_db = gain_tenths_db;
    for (const int input_dbm : dmr_rpt::rx_calibration_required_inputs(band)) {
        curve.points.push_back({
            input_dbm,
            static_cast<double>(input_dbm) - 20.0 + gain_tenths_db / 10.0,
            snr_db,
            {}});
    }
    return curve;
}

void test_three_range_calibration()
{
    dmr_rpt::RxSignalCalibrationConfig config;
    config.low[0] = make_curve(dmr_rpt::RxCalibrationBand::Low, 0);
    config.medium[0] = make_curve(dmr_rpt::RxCalibrationBand::Medium, 250);
    config.high[0] = make_curve(dmr_rpt::RxCalibrationBand::High, 500);

    require(dmr_rpt::rx_calibration_required_inputs(
                dmr_rpt::RxCalibrationBand::High).front() == -125 &&
            dmr_rpt::rx_calibration_required_inputs(
                dmr_rpt::RxCalibrationBand::High).back() == -75 &&
            dmr_rpt::rx_calibration_required_inputs(
                dmr_rpt::RxCalibrationBand::High).size() == 11U,
            "high range must cover -125 through -75 dBm");
    require(dmr_rpt::rx_calibration_curve_complete(
                config.low[0], dmr_rpt::RxCalibrationBand::Low) &&
            dmr_rpt::rx_calibration_curve_complete(
                config.medium[0], dmr_rpt::RxCalibrationBand::Medium) &&
            dmr_rpt::rx_calibration_curve_complete(
                config.high[0], dmr_rpt::RxCalibrationBand::High),
            "all three calibrated ranges must be complete");

    for (const auto item : {std::pair<int, std::int32_t>{-20, 0},
                            {-65, 250}, {-125, 500}}) {
        const double measured_dbfs = static_cast<double>(item.first) - 20.0 +
            item.second / 10.0;
        const auto reading = dmr_rpt::rx_calibration_reading(
            config, 0, item.second, measured_dbfs);
        require(reading.calibrated && reading.rssi_dbm,
                "three-range reading is unavailable");
        require(std::abs(*reading.rssi_dbm - item.first) < 0.001,
                "three-range reading changed the RSSI scale");
    }

    auto weak_snr = make_curve(dmr_rpt::RxCalibrationBand::High, 500, 11.9);
    require(!dmr_rpt::rx_calibration_curve_complete(
                weak_snr, dmr_rpt::RxCalibrationBand::High),
            "high range must reject SNR below 12 dB");
}

} // namespace

int main()
{
    try {
        test_three_range_calibration();
        std::cout << "RX signal calibration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RX signal calibration tests: " << error.what() << '\n';
        return 1;
    }
}
