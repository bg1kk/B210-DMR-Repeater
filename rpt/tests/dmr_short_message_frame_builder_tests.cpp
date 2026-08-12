// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_b210/dmr_short_message_frame_builder.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Burst = dmr_b210::ShortMessageFrameBuilder::Burst;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool hamming_15_11_valid(const bool* data)
{
    return data[11] == (data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[5] ^ data[7] ^ data[8]) &&
           data[12] == (data[1] ^ data[2] ^ data[3] ^ data[4] ^ data[6] ^ data[8] ^ data[9]) &&
           data[13] == (data[2] ^ data[3] ^ data[4] ^ data[5] ^ data[7] ^ data[9] ^ data[10]) &&
           data[14] == (data[0] ^ data[1] ^ data[2] ^ data[4] ^ data[6] ^ data[7] ^ data[10]);
}

bool hamming_13_9_valid(const bool* data)
{
    return data[9] == (data[0] ^ data[1] ^ data[3] ^ data[5] ^ data[6]) &&
           data[10] == (data[0] ^ data[1] ^ data[2] ^ data[4] ^ data[6] ^ data[7]) &&
           data[11] == (data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[5] ^ data[7] ^ data[8]) &&
           data[12] == (data[0] ^ data[2] ^ data[4] ^ data[5] ^ data[8]);
}

std::array<std::uint8_t, 12> decode_bptc_payload(const Burst& burst)
{
    std::array<bool, 264> frame_bits{};
    for (std::size_t index = 0; index < burst.size(); ++index) {
        frame_bits[index * 2U] = (burst[index] & 0x02U) != 0U;
        frame_bits[index * 2U + 1U] = (burst[index] & 0x01U) != 0U;
    }

    std::array<bool, 196> raw{};
    for (unsigned index = 0; index < 98U; ++index) {
        raw[index] = frame_bits[index];
    }
    for (unsigned index = 98U; index < raw.size(); ++index) {
        raw[index] = frame_bits[index + 68U];
    }

    std::array<bool, 196> deinterleaved{};
    for (unsigned index = 0; index < deinterleaved.size(); ++index) {
        deinterleaved[index] = raw[(index * 181U) % 196U];
    }
    for (unsigned row = 0; row < 9U; ++row) {
        require(hamming_15_11_valid(deinterleaved.data() + row * 15U + 1U),
                "BPTC row parity failed");
    }
    for (unsigned column = 0; column < 15U; ++column) {
        std::array<bool, 13> values{};
        for (unsigned row = 0; row < values.size(); ++row) {
            values[row] = deinterleaved[column + 1U + row * 15U];
        }
        require(hamming_13_9_valid(values.data()), "BPTC column parity failed");
    }

    std::array<std::uint8_t, 12> payload{};
    constexpr std::array<std::pair<unsigned, unsigned>, 9> ranges{{
        {4U, 11U}, {16U, 26U}, {31U, 41U}, {46U, 56U}, {61U, 71U},
        {76U, 86U}, {91U, 101U}, {106U, 116U}, {121U, 131U}
    }};
    unsigned bit = 0;
    for (const auto& range : ranges) {
        for (unsigned index = range.first; index <= range.second; ++index) {
            payload[bit / 8U] = static_cast<std::uint8_t>(
                (payload[bit / 8U] << 1U) | (deinterleaved[index] ? 1U : 0U));
            ++bit;
        }
    }
    return payload;
}

void test_round_trip(const std::array<std::uint8_t, 12>& payload, bool header)
{
    const Burst burst = header
        ? dmr_b210::ShortMessageFrameBuilder::data_header_burst(payload, 1U)
        : dmr_b210::ShortMessageFrameBuilder::rate_half_data_burst(payload, 1U);
    require(decode_bptc_payload(burst) == payload, "BPTC payload did not round trip");
}

} // namespace

int main()
{
    try {
        test_round_trip({0x0DU, 0xA0U, 0x14U, 0x00U, 0x00U, 0x01U,
                         0x00U, 0x07U, 0xD2U, 0x00U, 0x00U, 0x00U}, true);
        test_round_trip({0x00U, 0x42U, 0x00U, 0x32U, 0x00U, 0x31U,
                         0x00U, 0x30U, 0x00U, 0x54U, 0x00U, 0x58U}, false);
        test_round_trip({0x00U, 0x30U, 0x00U, 0x32U, 0x00U, 0x00U,
                         0x00U, 0x00U, 0xDEU, 0xADU, 0xBEU, 0xEFU}, false);
        std::cout << "DMR short-message BPTC tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DMR short-message BPTC tests failed: " << error.what() << '\n';
        return 1;
    }
}
