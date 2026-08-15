// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/io_status.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeGpio final : public dmr_rpt::B210GpioAdapter {
public:
    dmr_rpt::GpioCapability capability(const std::string& bank) override
    {
        return {bank, {0, 1, 2, 3, 4, 5}, {0, 1, 2, 3, 4, 5}};
    }

    bool configure_output(const std::string&, int pin) override
    {
        configured.push_back(pin);
        return true;
    }

    bool write(const std::string&, int pin, dmr_rpt::IoLevel level) override
    {
        levels[pin] = level;
        return true;
    }

    bool configure_input(const std::string&, int pin) override
    {
        configured_inputs.push_back(pin);
        return true;
    }

    std::optional<std::uint32_t> read_mask(
        const std::string&, std::uint32_t mask) override
    {
        read_masks.push_back(mask);
        return fail_read ? std::nullopt
                         : std::optional<std::uint32_t>(input_value & mask);
    }

    bool fail_read = false;
    std::uint32_t input_value = 0;
    std::vector<int> configured;
    std::vector<int> configured_inputs;
    std::map<int, dmr_rpt::IoLevel> levels;
    std::vector<std::uint32_t> read_masks;
};

} // namespace

int main()
{
    try {
        dmr_rpt::RxFrontendConditioningConfig config;
        config.enabled = true;
        config.low_attenuation_db = {0.0, 6.0, 12.0, 18.0};
        config.medium_attenuation_db = {0.0, 8.0, 16.0, 24.0};
        config.high_attenuation_db = {0.0, 10.0, 20.0, 30.0};

        FakeGpio gpio;
        gpio.input_value = (std::uint32_t{1} << 4) |
            (std::uint32_t{1} << 5);
        dmr_rpt::B210FrontendStageController controller(config, gpio);
        controller.initialize("low");
        require(controller.state().gpio_healthy, "initialization failed");
        require(controller.state().stage == 3 &&
                    controller.state().attenuation_db == 18.0,
                "input code 11 did not select low-range stage3");
        require(gpio.configured_inputs == std::vector<int>({4, 5}),
                "IO4 and IO5 were not configured as inputs");
        require(gpio.read_masks.size() == 1U,
                "stage inputs were not read together");

        gpio.input_value = std::uint32_t{1} << 4;
        require(controller.poll("medium"), "medium stage read failed");
        require(controller.state().gpio_code == 1 &&
                    controller.state().attenuation_db == 8.0,
                "medium attenuation lookup failed");
        gpio.input_value = std::uint32_t{1} << 5;
        require(controller.poll("high"), "high stage read failed");
        require(controller.state().gpio_code == 2 &&
                    controller.state().attenuation_db == 20.0,
                "high attenuation lookup failed");

        FakeGpio failing_gpio;
        failing_gpio.fail_read = true;
        dmr_rpt::B210FrontendStageController failing(config, failing_gpio);
        failing.initialize("low");
        require(!failing.state().gpio_healthy,
                "GPIO input read failure did not latch a fault");

        std::cout << "Front-end conditioning tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
