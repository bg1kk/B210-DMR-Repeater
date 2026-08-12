// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/io_status.h"

#include <algorithm>
#include <set>
#include <utility>

namespace dmr_rpt {
namespace {

bool contains_pin(const std::vector<int>& pins, int pin)
{
    return std::find(pins.begin(), pins.end(), pin) != pins.end();
}

} // namespace

B210IoStatusController::B210IoStatusController(IoStatusConfig config,
                                               B210GpioAdapter& gpio)
    : config_(std::move(config))
    , gpio_(gpio)
{
    state_.enabled = config_.enabled;
    state_.bank = config_.gpio_bank;
    state_.active_level = config_.active_level;
    state_.idle_level = config_.idle_level;
}

void B210IoStatusController::initialize(std::int64_t)
{
    state_.pins.clear();
    state_.enabled = config_.enabled;
    if (!config_.enabled) {
        state_.gpio_healthy = true;
        state_.last_error.clear();
        return;
    }

    GpioCapability capability;
    try {
        capability = gpio_.capability(config_.gpio_bank);
    } catch (const std::exception& error) {
        fault(error.what());
        return;
    }
    if (capability.bank != config_.gpio_bank) {
        fault("GPIO bank unavailable: " + config_.gpio_bank);
        return;
    }

    for (const IoPinConfig& pin_config : config_.pins) {
        if (!contains_pin(capability.available_pins, pin_config.io) ||
            !contains_pin(capability.output_capable_pins, pin_config.io)) {
            fault("GPIO pin is not output capable: IO" + std::to_string(pin_config.io));
            return;
        }
        if (!gpio_.configure_output(config_.gpio_bank, pin_config.io)) {
            fault("failed to configure GPIO output IO" + std::to_string(pin_config.io));
            return;
        }
        if (!gpio_.write(config_.gpio_bank, pin_config.io, IoLevel::High)) {
            fault("failed to set idle-high GPIO IO" + std::to_string(pin_config.io));
            return;
        }

        IoPinRuntime runtime;
        runtime.logical_name = pin_config.logical_name;
        runtime.pin = pin_config.io;
        runtime.mapped_rx_channel = pin_config.rx_channel;
        runtime.mapped_tx_channel = pin_config.tx_channel;
        runtime.level = IoLevel::High;
        runtime.asserted = false;
        runtime.release_delay_ms = pin_config.rx_channel
            ? config_.rx_release_delay_ms
            : config_.tx_release_delay_ms;
        state_.pins.push_back(runtime);
    }
    state_.gpio_healthy = true;
    state_.last_error.clear();
}

void B210IoStatusController::on_rx_activity(int rx_channel,
                                            bool active,
                                            bool qualified,
                                            std::int64_t now_ms)
{
    if (!config_.enabled || !state_.gpio_healthy) {
        return;
    }
    for (IoPinRuntime& pin : state_.pins) {
        if (pin.mapped_rx_channel && *pin.mapped_rx_channel == rx_channel) {
            if (active && qualified) {
                assert_pin(pin, now_ms);
            } else {
                schedule_release(pin, now_ms);
            }
        }
    }
}

void B210IoStatusController::on_tx_ptt(int tx_channel,
                                       bool ptt_asserted,
                                       std::int64_t now_ms)
{
    if (!config_.enabled || !state_.gpio_healthy) {
        return;
    }
    for (IoPinRuntime& pin : state_.pins) {
        if (pin.mapped_tx_channel && *pin.mapped_tx_channel == tx_channel) {
            if (ptt_asserted) {
                assert_pin(pin, now_ms);
            } else {
                schedule_release(pin, now_ms);
            }
        }
    }
}

void B210IoStatusController::poll(std::int64_t now_ms)
{
    if (!config_.enabled || !state_.gpio_healthy) {
        return;
    }
    for (IoPinRuntime& pin : state_.pins) {
        if (pin.release_deadline_monotonic_ms &&
            *pin.release_deadline_monotonic_ms <= now_ms) {
            write_pin(pin, IoLevel::High);
            pin.asserted = false;
            pin.release_deadline_monotonic_ms.reset();
        }
    }
}

void B210IoStatusController::release_all_high()
{
    if (!config_.enabled) {
        return;
    }
    for (IoPinRuntime& pin : state_.pins) {
        const bool ok = gpio_.write(config_.gpio_bank, pin.pin, IoLevel::High);
        pin.level = IoLevel::High;
        pin.asserted = false;
        pin.release_deadline_monotonic_ms.reset();
        if (!ok) {
            state_.gpio_healthy = false;
            state_.last_error = "failed to release IO" + std::to_string(pin.pin) + " high";
        }
    }
}

const IoStatusState& B210IoStatusController::state() const
{
    return state_;
}

void B210IoStatusController::assert_pin(IoPinRuntime& pin, std::int64_t now_ms)
{
    write_pin(pin, IoLevel::Low);
    pin.asserted = true;
    pin.last_asserted_monotonic_ms = now_ms;
    pin.release_deadline_monotonic_ms.reset();
}

void B210IoStatusController::schedule_release(IoPinRuntime& pin,
                                              std::int64_t now_ms)
{
    if (!pin.asserted) {
        return;
    }
    pin.release_deadline_monotonic_ms =
        now_ms + static_cast<std::int64_t>(pin.release_delay_ms);
}

void B210IoStatusController::write_pin(IoPinRuntime& pin, IoLevel level)
{
    if (!gpio_.write(config_.gpio_bank, pin.pin, level)) {
        fault("failed to write GPIO IO" + std::to_string(pin.pin));
        return;
    }
    pin.level = level;
}

void B210IoStatusController::fault(const std::string& message)
{
    state_.gpio_healthy = false;
    state_.last_error = message;
    for (IoPinRuntime& pin : state_.pins) {
        gpio_.write(config_.gpio_bank, pin.pin, IoLevel::High);
        pin.level = IoLevel::High;
        pin.asserted = false;
        pin.release_deadline_monotonic_ms.reset();
    }
}

const char* to_string(IoLevel level)
{
    return level == IoLevel::Low ? "low" : "high";
}

} // namespace dmr_rpt
