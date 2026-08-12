// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_slot.h"
#include "golay2087.h"
#include "hamming.h"
#include <array>
#include <utility>

namespace gr {
namespace dmr {

dmr_slot::dmr_slot(int slot, int debug)
    : d_slot(slot), d_debug(debug), d_color_code(-1)
{
}

void dmr_slot::set_lc_callback(lc_callback callback)
{
    d_callback = std::move(callback);
}

void dmr_slot::set_cc(uint8_t color_code)
{
    d_color_code = color_code & 0x0F;
}

uint8_t dmr_slot::get_cc() const
{
    return d_color_code < 0 ? 0 : static_cast<uint8_t>(d_color_code);
}

bool dmr_slot::load_slot(const uint8_t slot_bits[264], uint64_t sync_type)
{
    (void)sync_type;
    (void)d_debug;

    bit_vector embedded_signaling;
    for (int index = 108; index < 116; ++index) {
        embedded_signaling.push_back(slot_bits[index] != 0);
    }
    for (int index = 148; index < 156; ++index) {
        embedded_signaling.push_back(slot_bits[index] != 0);
    }

    const unsigned int qr_errors = CQR1676::decode(embedded_signaling);
    if (qr_errors > 2U) {
        return false;
    }

    const int embedded_color_code =
        (embedded_signaling[0] << 3) | (embedded_signaling[1] << 2) |
        (embedded_signaling[2] << 1) | embedded_signaling[3];
    if (d_color_code < 0) {
        d_color_code = embedded_color_code;
    } else if (d_color_code != embedded_color_code) {
        return false;
    }

    const int lcss =
        (embedded_signaling[5] << 1) | embedded_signaling[6];
    auto append_fragment = [&]() {
        for (int index = 116; index < 148; ++index) {
            d_fragments.push_back(slot_bits[index] != 0);
        }
    };

    switch (lcss) {
    case 0:
        d_fragments.clear();
        return true;
    case 1:
        d_fragments.clear();
        append_fragment();
        return true;
    case 3:
        if (!d_fragments.empty()) {
            append_fragment();
        }
        return true;
    case 2:
        if (d_fragments.empty()) {
            return false;
        }
        append_fragment();
        return decode_embedded_lc();
    default:
        return false;
    }
}

bool dmr_slot::decode_embedded_lc()
{
    if (d_fragments.size() != 128) {
        d_fragments.clear();
        return false;
    }

    std::array<bool, 128> data{};
    unsigned int destination = 0;
    for (bool bit : d_fragments) {
        data[destination] = bit;
        destination += 16;
        if (destination > 127) {
            destination -= 127;
        }
    }
    d_fragments.clear();

    for (unsigned int row = 0; row < 112; row += 16) {
        if (!CHamming::decode16114(data.data() + row)) {
            return false;
        }
    }

    for (unsigned int column = 0; column < 16; ++column) {
        bool parity = false;
        for (unsigned int row = 0; row < 128; row += 16) {
            parity ^= data[row + column];
        }
        if (parity) {
            return false;
        }
    }

    std::vector<bool> payload;
    payload.reserve(72);
    const std::array<std::pair<unsigned int, unsigned int>, 7> ranges{{
        {0, 11}, {16, 27}, {32, 42}, {48, 58},
        {64, 74}, {80, 90}, {96, 106},
    }};
    for (const auto& range : ranges) {
        for (unsigned int index = range.first; index < range.second; ++index) {
            payload.push_back(data[index]);
        }
    }

    std::array<uint8_t, 9> lc_bytes{};
    for (std::size_t byte = 0; byte < lc_bytes.size(); ++byte) {
        uint8_t value = 0;
        for (std::size_t bit = 0; bit < 8; ++bit) {
            value = static_cast<uint8_t>(
                (value << 1U) | (payload[byte * 8 + bit] ? 1U : 0U));
        }
        lc_bytes[byte] = value;
    }

    const uint8_t received_crc = static_cast<uint8_t>(
        (data[42] << 4) | (data[58] << 3) | (data[74] << 2) |
        (data[90] << 1) | data[106]);
    unsigned int crc_sum = 0;
    for (uint8_t value : lc_bytes) {
        crc_sum += value;
    }
    if ((crc_sum % 31U) != received_crc) {
        return false;
    }

    dmr_lc_data lc;
    lc.valid = true;
    lc.pf = (lc_bytes[0] >> 7) & 0x01;
    lc.flco = lc_bytes[0] & 0x3F;
    lc.fid = lc_bytes[1];
    lc.svcopt = lc_bytes[2];
    lc.dstaddr = (static_cast<uint32_t>(lc_bytes[3]) << 16) |
                 (static_cast<uint32_t>(lc_bytes[4]) << 8) |
                 lc_bytes[5];
    lc.srcaddr = (static_cast<uint32_t>(lc_bytes[6]) << 16) |
                 (static_cast<uint32_t>(lc_bytes[7]) << 8) |
                 lc_bytes[8];

    if (d_callback) {
        d_callback(d_slot, get_cc(), lc, false);
    }
    return true;
}

} // namespace dmr
} // namespace gr
