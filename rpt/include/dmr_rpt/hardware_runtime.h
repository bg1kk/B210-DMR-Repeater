// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "dmr_rpt/audit.h"
#include "dmr_rpt/config.h"
#include "dmr_rpt/network_protocol.h"
#include "dmr_rpt/rf_session.h"

namespace dmr_rpt {

class HardwareB210SessionFactory final : public B210SessionFactory {
public:
    HardwareB210SessionFactory(ValidatedConfig config,
                               OperationAuditLogger& audit,
                               bool rx_diagnostic = false,
                               std::shared_ptr<NetworkEventSink> network = {},
                               std::shared_ptr<std::atomic_bool>
                                   forwarding_enabled = {},
                               std::shared_ptr<RxSignalCalibrationRuntime>
                                   calibration = {},
                               std::function<void()> recording_storage_update = {});

    std::unique_ptr<B210Session> create() override;

private:
    ValidatedConfig config_;
    OperationAuditLogger& audit_;
    bool rx_diagnostic_ = false;
    std::shared_ptr<NetworkEventSink> network_;
    std::shared_ptr<std::atomic_bool> forwarding_enabled_;
    std::shared_ptr<RxSignalCalibrationRuntime> calibration_;
    std::function<void()> recording_storage_update_;
};

} // namespace dmr_rpt
