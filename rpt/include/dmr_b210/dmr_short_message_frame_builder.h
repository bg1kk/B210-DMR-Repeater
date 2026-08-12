// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <cstdint>

namespace dmr_b210 {

class ShortMessageFrameBuilder {
public:
    using Burst = std::array<std::uint8_t, 132>;

    static Burst data_header_burst(const std::array<std::uint8_t, 12>& payload,
                                   unsigned color_code);
    static Burst csbk_burst(const std::array<std::uint8_t, 12>& payload,
                            unsigned color_code);
    static Burst rate_half_data_burst(const std::array<std::uint8_t, 12>& payload,
                                      unsigned color_code);
};

} // namespace dmr_b210
