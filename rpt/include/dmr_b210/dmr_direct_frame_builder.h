// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dmr_b210 {

class DirectFrameBuilder {
public:
    static constexpr std::size_t kAmbeDibits = 36;
    static constexpr std::size_t kBurstDibits = 132;
    static constexpr std::size_t kSlotDibits = 144;
    static constexpr std::size_t kFrameDibits = 288;
    using Burst = std::array<std::uint8_t, kBurstDibits>;

    DirectFrameBuilder(unsigned source_id,
                       unsigned target_id,
                       unsigned color_code,
                       unsigned slot);

    const Burst& header_burst() const;
    const Burst& terminator_burst() const;
    Burst voice_burst(const std::uint8_t* ambe_dibits,
                      unsigned voice_index) const;
    void emit_frame(const Burst& burst,
                    std::uint8_t* output,
                    float* gate) const;

private:
    unsigned slot_ = 1;
    Burst header_{};
    Burst terminator_{};
    std::array<std::uint8_t, 24> voice_sync_{};
    std::array<std::array<std::uint8_t, 24>, 5> embedded_signalling_{};
};

} // namespace dmr_b210
