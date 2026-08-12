// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "dmr_rpt/config.h"

namespace dmr_rpt {

enum class IoLevel {
    Low,
    High,
};

struct GpioCapability {
    std::string bank = "FP0";
    std::vector<int> available_pins;
    std::vector<int> output_capable_pins;
};

class B210GpioAdapter {
public:
    virtual ~B210GpioAdapter() = default;
    virtual GpioCapability capability(const std::string& bank) = 0;
    virtual bool configure_output(const std::string& bank, int pin) = 0;
    virtual bool write(const std::string& bank, int pin, IoLevel level) = 0;
};

struct IoPinRuntime {
    std::string logical_name;
    int pin = -1;
    std::optional<int> mapped_rx_channel;
    std::optional<int> mapped_tx_channel;
    IoLevel level = IoLevel::High;
    bool asserted = false;
    std::int64_t last_asserted_monotonic_ms = 0;
    int release_delay_ms = 0;
    std::optional<std::int64_t> release_deadline_monotonic_ms;
};

struct IoStatusState {
    bool enabled = true;
    std::string bank = "FP0";
    std::string active_level = "low";
    std::string idle_level = "high";
    bool gpio_healthy = true;
    std::string last_error;
    std::string configuration_activation_state = "running";
    std::vector<IoPinRuntime> pins;
};

class B210IoStatusController {
public:
    B210IoStatusController(IoStatusConfig config, B210GpioAdapter& gpio);

    void initialize(std::int64_t now_ms);
    void on_rx_activity(int rx_channel,
                        bool active,
                        bool qualified,
                        std::int64_t now_ms);
    void on_tx_ptt(int tx_channel,
                   bool ptt_asserted,
                   std::int64_t now_ms);
    void poll(std::int64_t now_ms);
    void release_all_high();
    const IoStatusState& state() const;

private:
    void assert_pin(IoPinRuntime& pin, std::int64_t now_ms);
    void schedule_release(IoPinRuntime& pin, std::int64_t now_ms);
    void write_pin(IoPinRuntime& pin, IoLevel level);
    void fault(const std::string& message);

    IoStatusConfig config_;
    B210GpioAdapter& gpio_;
    IoStatusState state_;
};

const char* to_string(IoLevel level);

} // namespace dmr_rpt
