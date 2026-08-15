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

    bool write_mask(const std::string&, std::uint32_t value,
                    std::uint32_t mask) override
    {
        masked_writes.push_back({value, mask});
        return !fail_masked_write;
    }

    bool fail_masked_write = false;
    std::vector<int> configured;
    std::map<int, dmr_rpt::IoLevel> levels;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> masked_writes;
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
        dmr_rpt::B210FrontendStageController controller(config, gpio);
        controller.initialize("low");
        require(controller.state().gpio_healthy, "initialization failed");
        require(controller.state().stage == 3 &&
                    controller.state().attenuation_db == 18.0,
                "safe startup did not select maximum low-range attenuation");
        require(gpio.configured == std::vector<int>({4, 5}),
                "IO4 and IO5 were not configured");
        require(gpio.masked_writes.size() == 1U,
                "startup stage was not one atomic write");

        require(controller.set_stage("medium", 1), "medium stage failed");
        require(controller.state().gpio_code == 1 &&
                    controller.state().attenuation_db == 8.0,
                "medium attenuation lookup failed");
        require(controller.set_stage("high", 2), "high stage failed");
        require(controller.state().gpio_code == 2 &&
                    controller.state().attenuation_db == 20.0,
                "high attenuation lookup failed");

        controller.release_stage_zero();
        require(controller.state().stage == 0 &&
                    controller.state().attenuation_db == 0.0,
                "release did not restore stage zero");

        FakeGpio failing_gpio;
        failing_gpio.fail_masked_write = true;
        dmr_rpt::B210FrontendStageController failing(config, failing_gpio);
        failing.initialize("low");
        require(!failing.state().gpio_healthy,
                "masked write failure did not latch a GPIO fault");

        std::cout << "Front-end conditioning tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

