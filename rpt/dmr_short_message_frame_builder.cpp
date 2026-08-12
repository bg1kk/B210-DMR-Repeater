// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_b210/dmr_short_message_frame_builder.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace dmr_b210 {
namespace {

constexpr std::uint8_t kDataHeaderType = 0x06U;
constexpr std::uint8_t kCsbkType = 0x03U;
constexpr std::uint8_t kRateHalfDataType = 0x07U;
constexpr std::uint64_t kMsDataSync = 0xD5D7F77FD757ULL;

bool parity(std::uint32_t value)
{
    bool result = false;
    while (value != 0U) {
        result = !result;
        value &= value - 1U;
    }
    return result;
}

std::uint32_t polynomial_remainder(std::uint32_t value, std::uint32_t polynomial)
{
    int polynomial_degree = 31;
    while ((polynomial & (1U << polynomial_degree)) == 0U) {
        --polynomial_degree;
    }
    for (int degree = 31; degree >= polynomial_degree; --degree) {
        if ((value & (1U << degree)) != 0U) {
            value ^= polynomial << (degree - polynomial_degree);
        }
    }
    return value;
}

void byte_to_bits(std::uint8_t value, bool* bits)
{
    for (unsigned bit = 0; bit < 8U; ++bit) {
        bits[bit] = ((value >> (7U - bit)) & 1U) != 0U;
    }
}

std::uint8_t bits_to_byte(const bool* bits)
{
    std::uint8_t value = 0;
    for (unsigned bit = 0; bit < 8U; ++bit) {
        value = static_cast<std::uint8_t>((value << 1U) | (bits[bit] ? 1U : 0U));
    }
    return value;
}

void hamming_15_11(bool* data)
{
    data[11] = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[5] ^ data[7] ^ data[8];
    data[12] = data[1] ^ data[2] ^ data[3] ^ data[4] ^ data[6] ^ data[8] ^ data[9];
    data[13] = data[2] ^ data[3] ^ data[4] ^ data[5] ^ data[7] ^ data[9] ^ data[10];
    data[14] = data[0] ^ data[1] ^ data[2] ^ data[4] ^ data[6] ^ data[7] ^ data[10];
}

void hamming_13_9(bool* data)
{
    data[9] = data[0] ^ data[1] ^ data[3] ^ data[5] ^ data[6];
    data[10] = data[0] ^ data[1] ^ data[2] ^ data[4] ^ data[6] ^ data[7];
    data[11] = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[5] ^ data[7] ^ data[8];
    data[12] = data[0] ^ data[2] ^ data[4] ^ data[5] ^ data[8];
}

void bptc19696(const std::array<std::uint8_t, 12>& input,
               std::array<std::uint8_t, 33>& output)
{
    std::array<bool, 96> payload{};
    for (unsigned index = 0; index < input.size(); ++index) {
        byte_to_bits(input[index], payload.data() + index * 8U);
    }

    std::array<bool, 196> deinterleaved{};
    unsigned position = 0;
    const std::array<std::pair<unsigned, unsigned>, 9> ranges{{
        {4U, 11U}, {16U, 26U}, {31U, 41U}, {46U, 56U}, {61U, 71U},
        {76U, 86U}, {91U, 101U}, {106U, 116U}, {121U, 131U}
    }};
    for (const auto& range : ranges) {
        for (unsigned index = range.first; index <= range.second; ++index) {
            deinterleaved[index] = payload[position++];
        }
    }
    for (unsigned row = 0; row < 9U; ++row) {
        hamming_15_11(deinterleaved.data() + row * 15U + 1U);
    }
    for (unsigned column = 0; column < 15U; ++column) {
        std::array<bool, 13> values{};
        for (unsigned row = 0; row < 13U; ++row) {
            values[row] = deinterleaved[column + 1U + row * 15U];
        }
        hamming_13_9(values.data());
        for (unsigned row = 0; row < 13U; ++row) {
            deinterleaved[column + 1U + row * 15U] = values[row];
        }
    }

    std::array<bool, 196> raw{};
    for (unsigned index = 0; index < raw.size(); ++index) {
        raw[(index * 181U) % 196U] = deinterleaved[index];
    }
    for (unsigned byte = 0; byte < 12U; ++byte) {
        output[byte] = bits_to_byte(raw.data() + byte * 8U);
    }
    output[12] = static_cast<std::uint8_t>((output[12] & 0x3FU) |
        (bits_to_byte(raw.data() + 96U) & 0xC0U));
    output[20] = static_cast<std::uint8_t>((output[20] & 0xFCU) |
        ((bits_to_byte(raw.data() + 96U) >> 4U) & 0x03U));
    for (unsigned byte = 0; byte < 12U; ++byte) {
        output[21U + byte] = bits_to_byte(raw.data() + 100U + byte * 8U);
    }
}

void add_slot_type(std::array<std::uint8_t, 33>& data,
                   unsigned color_code,
                   std::uint8_t data_type)
{
    const std::uint8_t value = static_cast<std::uint8_t>((color_code << 4U) | data_type);
    const std::uint32_t remainder = polynomial_remainder(
        static_cast<std::uint32_t>(value) << 11U, 0xC75U);
    const std::uint32_t code19 = (static_cast<std::uint32_t>(value) << 11U) | remainder;
    const std::array<std::uint8_t, 3> slot_type{
        value,
        static_cast<std::uint8_t>(remainder >> 3U),
        static_cast<std::uint8_t>(((remainder & 0x07U) << 5U) |
                                  (parity(code19) ? 0x10U : 0U))
    };
    data[12] = static_cast<std::uint8_t>((data[12] & 0xC0U) |
                                          ((slot_type[0] >> 2U) & 0x3FU));
    data[13] = static_cast<std::uint8_t>((data[13] & 0x0FU) |
                                          ((slot_type[0] << 6U) & 0xC0U) |
                                          ((slot_type[1] >> 2U) & 0x30U));
    data[19] = static_cast<std::uint8_t>((data[19] & 0xF0U) |
                                          ((slot_type[1] >> 2U) & 0x0FU));
    data[20] = static_cast<std::uint8_t>((data[20] & 0x03U) |
                                          ((slot_type[1] << 6U) & 0xC0U) |
                                          ((slot_type[2] >> 2U) & 0x3CU));
}

ShortMessageFrameBuilder::Burst make_burst(
    const std::array<std::uint8_t, 12>& payload,
    unsigned color_code,
    std::uint8_t data_type)
{
    if (color_code > 15U) {
        throw std::invalid_argument("DMR color code must be 0..15");
    }
    std::array<std::uint8_t, 33> bytes{};
    bptc19696(payload, bytes);
    add_slot_type(bytes, color_code, data_type);

    ShortMessageFrameBuilder::Burst dibits{};
    for (unsigned index = 0; index < bytes.size(); ++index) {
        for (unsigned pair = 0; pair < 4U; ++pair) {
            dibits[index * 4U + pair] = static_cast<std::uint8_t>(
                (bytes[index] >> (6U - pair * 2U)) & 0x03U);
        }
    }
    for (unsigned index = 0; index < 24U; ++index) {
        dibits[54U + index] = static_cast<std::uint8_t>(
            (kMsDataSync >> (46U - index * 2U)) & 0x03U);
    }
    return dibits;
}

} // namespace

ShortMessageFrameBuilder::Burst ShortMessageFrameBuilder::data_header_burst(
    const std::array<std::uint8_t, 12>& payload, unsigned color_code)
{
    return make_burst(payload, color_code, kDataHeaderType);
}

ShortMessageFrameBuilder::Burst ShortMessageFrameBuilder::csbk_burst(
    const std::array<std::uint8_t, 12>& payload, unsigned color_code)
{
    return make_burst(payload, color_code, kCsbkType);
}

ShortMessageFrameBuilder::Burst ShortMessageFrameBuilder::rate_half_data_burst(
    const std::array<std::uint8_t, 12>& payload, unsigned color_code)
{
    return make_burst(payload, color_code, kRateHalfDataType);
}

} // namespace dmr_b210
