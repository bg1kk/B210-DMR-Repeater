// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_b210/dmr_burst_symbol_sampler.h"

#include <gnuradio/blocks/vector_sink.h>
#include <gnuradio/blocks/vector_source.h>
#include <gnuradio/top_block.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::array<float, 4> kLevels {
    -1.0F, -1.0F / 3.0F, 1.0F / 3.0F, 1.0F
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<float> make_frame_levels()
{
    constexpr std::uint64_t sync_pattern = 0xDFF57D75DF5DULL;
    constexpr std::array<std::uint8_t, 4> dibit_to_level {2U, 3U, 1U, 0U};

    std::vector<float> frame(132U);
    for (std::size_t symbol = 0; symbol < frame.size(); ++symbol) {
        frame[symbol] = kLevels[symbol % kLevels.size()];
    }
    for (std::size_t symbol = 0; symbol < 24U; ++symbol) {
        const unsigned shift = static_cast<unsigned>((23U - symbol) * 2U);
        const std::uint8_t dibit = static_cast<std::uint8_t>(
            (sync_pattern >> shift) & 0x3U);
        frame[54U + symbol] = kLevels[dibit_to_level[dibit]];
    }
    return frame;
}

std::vector<float> make_balanced_symbols(std::size_t count)
{
    std::vector<float> symbols(count);
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        symbols[index] = kLevels[index % kLevels.size()];
    }
    return symbols;
}

void append_oversampled(std::vector<float>& samples,
                        const std::vector<float>& symbols)
{
    for (const float symbol : symbols) {
        samples.insert(samples.end(), 10U, symbol);
    }
}

std::vector<float> run_sampler(const std::vector<float>& samples)
{
    const auto source = gr::blocks::vector_source_f::make(samples, false);
    const auto sampler = dmr_b210::SharedDmrBurstSymbolSampler::make(false);
    const auto sink = gr::blocks::vector_sink_f::make();
    const auto flowgraph = gr::make_top_block("dmr sampler test");
    flowgraph->connect(source, 0, sampler, 0);
    flowgraph->connect(sampler, 0, sink, 0);
    flowgraph->run();
    return sink->data();
}

void require_frame_output(const std::vector<float>& output,
                          const std::vector<float>& expected)
{
    require(output.size() >= 264U,
            "sampler did not emit one complete burst and metadata block");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto dibit_for_symbol = [](float sample) {
            if (sample > 2.0F / 3.0F) return 1U;
            if (sample > 0.0F) return 0U;
            if (sample > -2.0F / 3.0F) return 2U;
            return 3U;
        };
        require(dibit_for_symbol(output[index]) ==
                    dibit_for_symbol(expected[index]),
                "sampler emitted incorrect symbol decision at index " +
                std::to_string(index));
    }
}

void test_guard_acquisition()
{
    const std::vector<float> frame = make_frame_levels();
    std::vector<float> samples(40U, 0.0F);
    append_oversampled(samples, frame);
    samples.insert(samples.end(), 40U, 0.0F);
    require_frame_output(run_sampler(samples), frame);
}

void test_continuous_signal_acquisition()
{
    const std::vector<float> frame = make_frame_levels();
    std::vector<float> samples;
    append_oversampled(samples, make_balanced_symbols(120U));
    append_oversampled(samples, frame);
    append_oversampled(samples, make_balanced_symbols(60U));
    require(samples.size() > 3000U, "continuous test vector is too short");
    require_frame_output(run_sampler(samples), frame);
}

void test_reacquisition_after_idle_slot()
{
    const std::vector<float> frame = make_frame_levels();
    std::vector<float> samples(40U, 0.0F);
    append_oversampled(samples, frame);
    samples.insert(samples.end(), 40U, 0.0F);
    samples.insert(samples.end(), 12000U, 0.0F);
    append_oversampled(samples, frame);
    samples.insert(samples.end(), 40U, 0.0F);

    const std::vector<float> output = run_sampler(samples);
    require(output.size() >= 528U,
            "sampler did not reacquire DMR after three scheduled idle slots");
    require_frame_output(output, frame);
    require_frame_output(
        std::vector<float>(output.begin() + 264, output.end()), frame);
}

void test_continuous_noise_rejected()
{
    std::vector<float> samples;
    append_oversampled(samples, make_balanced_symbols(320U));
    require(run_sampler(samples).empty(),
            "sampler falsely acquired a continuous signal without DMR sync");
}

} // namespace

int main()
{
    try {
        std::cout << "test_guard_acquisition\n";
        test_guard_acquisition();
        std::cout << "test_continuous_signal_acquisition\n";
        test_continuous_signal_acquisition();
        std::cout << "test_reacquisition_after_idle_slot\n";
        test_reacquisition_after_idle_slot();
        std::cout << "test_continuous_noise_rejected\n";
        test_continuous_noise_rejected();
        std::cout << "DMR burst symbol sampler tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DMR burst symbol sampler tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
