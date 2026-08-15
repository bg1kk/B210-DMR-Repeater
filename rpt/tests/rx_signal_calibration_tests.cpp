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

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

dmr_rpt::RxSignalCalibrationCurve make_curve(
    dmr_rpt::RxCalibrationBand band, std::int32_t gain_tenths_db)
{
    dmr_rpt::RxSignalCalibrationCurve curve;
    curve.rx_gain_tenths_db = gain_tenths_db;
    for (const int input_dbm : dmr_rpt::rx_calibration_required_inputs(band)) {
        curve.points.push_back({
            input_dbm,
            static_cast<double>(input_dbm) - 20.0 + gain_tenths_db / 10.0,
            20.0,
            {}});
    }
    return curve;
}

void test_gain_compensation_over_80_db()
{
    dmr_rpt::RxSignalCalibrationConfig config;
    config.low[0] = make_curve(dmr_rpt::RxCalibrationBand::Low, 0);
    config.high[0] = make_curve(dmr_rpt::RxCalibrationBand::High, 250);

    for (int input_dbm = 0; input_dbm >= -80; input_dbm -= 5) {
        for (const std::int32_t gain_tenths_db : {0, 100, 200}) {
            const double measured_dbfs = static_cast<double>(input_dbm) - 20.0 +
                gain_tenths_db / 10.0;
            const dmr_rpt::RxCalibrationReading reading =
                dmr_rpt::rx_calibration_reading(
                    config, 0, gain_tenths_db, measured_dbfs);
            require(reading.calibrated && reading.rssi_dbm,
                    "gain-compensated reading is unavailable");
            require(std::abs(*reading.rssi_dbm - input_dbm) < 0.001,
                    "gain compensation changed the RSSI scale");
        }
    }

    const dmr_rpt::RxCalibrationReading nearest =
        dmr_rpt::rx_calibration_reading(config, 0, 200, -70.0);
    require(nearest.reference_gain_tenths_db ==
                std::optional<std::int32_t>(250) &&
                nearest.gain_compensation_db == std::optional<double>(5.0),
            "nearest reference gain was not selected");
}

} // namespace

int main()
{
    try {
        test_gain_compensation_over_80_db();
        std::cout << "RX signal calibration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RX signal calibration tests: " << error.what() << '\n';
        return 1;
    }
}
