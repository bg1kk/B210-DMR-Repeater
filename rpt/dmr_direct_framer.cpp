// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
/*
 * DMR direct-mode burst framing.
 *
 * The BPTC, Golay, QR, Reed-Solomon and embedded-LC layouts follow the
 * ETSI DMR implementation used by MMDVMHost and OP25 (GPL-2.0-or-later).
 */

#include "dmr_direct_framer.h"
#include "dmr_b210/dmr_direct_frame_builder.h"

#include <gnuradio/io_signature.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace dmr_b210 {
namespace {

constexpr int kAmbeDibits = 36;
constexpr int kBurstDibits = 132;
constexpr int kSlotDibits = 144;
constexpr int kFrameDibits = 288;
constexpr unsigned kHeaderBursts = 8;
constexpr unsigned kTerminatorBursts = 2;
constexpr uint8_t kVoiceLcHeader = 0x01;
constexpr uint8_t kTerminatorWithLc = 0x02;
constexpr uint8_t kVoiceHeaderMask = 0x96;
constexpr uint8_t kTerminatorMask = 0x99;

bool parity(uint32_t value)
{
    bool result = false;
    while (value != 0U) {
        result = !result;
        value &= value - 1U;
    }
    return result;
}

uint32_t polynomial_remainder(uint32_t value, uint32_t polynomial)
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

void byte_to_bits(uint8_t value, bool* bits)
{
    for (unsigned bit = 0; bit < 8; ++bit) {
        bits[bit] = ((value >> (7U - bit)) & 1U) != 0U;
    }
}

uint8_t bits_to_byte(const bool* bits)
{
    uint8_t value = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        value = static_cast<uint8_t>((value << 1U) | (bits[bit] ? 1U : 0U));
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

void hamming_16_11(bool* data)
{
    data[11] = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[5] ^ data[7] ^ data[8];
    data[12] = data[1] ^ data[2] ^ data[3] ^ data[4] ^ data[6] ^ data[8] ^ data[9];
    data[13] = data[2] ^ data[3] ^ data[4] ^ data[5] ^ data[7] ^ data[9] ^ data[10];
    data[14] = data[0] ^ data[1] ^ data[2] ^ data[4] ^ data[6] ^ data[7] ^ data[10];
    data[15] = data[0] ^ data[2] ^ data[5] ^ data[6] ^ data[8] ^ data[9] ^ data[10];
}

uint8_t gf256_multiply(uint8_t left, uint8_t right)
{
    uint8_t product = 0;
    while (right != 0U) {
        if ((right & 1U) != 0U) {
            product ^= left;
        }
        const bool carry = (left & 0x80U) != 0U;
        left <<= 1U;
        if (carry) {
            left ^= 0x1DU;
        }
        right >>= 1U;
    }
    return product;
}

std::array<uint8_t, 3> rs129(const std::array<uint8_t, 9>& message)
{
    std::array<uint8_t, 3> state{};
    constexpr std::array<uint8_t, 3> generator{ 64U, 56U, 14U };
    for (uint8_t byte : message) {
        const uint8_t value = byte ^ state[2];
        state[2] = state[1] ^ gf256_multiply(generator[2], value);
        state[1] = state[0] ^ gf256_multiply(generator[1], value);
        state[0] = gf256_multiply(generator[0], value);
    }
    return state;
}

std::array<uint8_t, 9> make_lc(unsigned source_id, unsigned target_id)
{
    return { 0x00U, 0x00U, 0x00U,
             static_cast<uint8_t>(target_id >> 16U),
             static_cast<uint8_t>(target_id >> 8U),
             static_cast<uint8_t>(target_id),
             static_cast<uint8_t>(source_id >> 16U),
             static_cast<uint8_t>(source_id >> 8U),
             static_cast<uint8_t>(source_id) };
}

void bptc19696(const std::array<uint8_t, 12>& input, std::array<uint8_t, 33>& output)
{
    std::array<bool, 96> payload{};
    for (unsigned index = 0; index < input.size(); ++index) {
        byte_to_bits(input[index], payload.data() + index * 8U);
    }

    std::array<bool, 196> deinterleaved{};
    unsigned position = 0;
    const std::array<std::pair<unsigned, unsigned>, 9> ranges{{
        { 4U, 11U }, { 16U, 26U }, { 31U, 41U }, { 46U, 56U },
        { 61U, 71U }, { 76U, 86U }, { 91U, 101U }, { 106U, 116U },
        { 121U, 131U }
    }};
    for (const auto& range : ranges) {
        for (unsigned index = range.first; index <= range.second; ++index) {
            deinterleaved[index] = payload[position++];
        }
    }

    for (unsigned row = 0; row < 9; ++row) {
        hamming_15_11(deinterleaved.data() + row * 15U + 1U);
    }
    for (unsigned column = 0; column < 15; ++column) {
        std::array<bool, 13> values{};
        for (unsigned row = 0; row < 13; ++row) {
            values[row] = deinterleaved[column + 1U + row * 15U];
        }
        hamming_13_9(values.data());
        for (unsigned row = 0; row < 13; ++row) {
            deinterleaved[column + 1U + row * 15U] = values[row];
        }
    }

    std::array<bool, 196> raw{};
    for (unsigned index = 0; index < raw.size(); ++index) {
        raw[(index * 181U) % 196U] = deinterleaved[index];
    }
    for (unsigned byte = 0; byte < 12; ++byte) {
        output[byte] = bits_to_byte(raw.data() + byte * 8U);
    }
    output[12] = static_cast<uint8_t>((output[12] & 0x3FU) |
                                     (bits_to_byte(raw.data() + 96U) & 0xC0U));
    output[20] = static_cast<uint8_t>((output[20] & 0xFCU) |
                                     ((bits_to_byte(raw.data() + 96U) >> 4U) & 0x03U));
    for (unsigned byte = 0; byte < 12; ++byte) {
        output[21U + byte] = bits_to_byte(raw.data() + 100U + byte * 8U);
    }
}

void add_slot_type(std::array<uint8_t, 33>& data, unsigned color_code, uint8_t data_type)
{
    const uint8_t value = static_cast<uint8_t>((color_code << 4U) | data_type);
    const uint32_t remainder = polynomial_remainder(static_cast<uint32_t>(value) << 11U,
                                                    0xC75U);
    const uint32_t code19 = (static_cast<uint32_t>(value) << 11U) | remainder;
    const bool extension = parity(code19);
    const std::array<uint8_t, 3> slot_type{
        value,
        static_cast<uint8_t>(remainder >> 3U),
        static_cast<uint8_t>(((remainder & 0x07U) << 5U) | (extension ? 0x10U : 0U))
    };

    data[12] = static_cast<uint8_t>((data[12] & 0xC0U) | ((slot_type[0] >> 2U) & 0x3FU));
    data[13] = static_cast<uint8_t>((data[13] & 0x0FU) | ((slot_type[0] << 6U) & 0xC0U) |
                                    ((slot_type[1] >> 2U) & 0x30U));
    data[19] = static_cast<uint8_t>((data[19] & 0xF0U) | ((slot_type[1] >> 2U) & 0x0FU));
    data[20] = static_cast<uint8_t>((data[20] & 0x03U) | ((slot_type[1] << 6U) & 0xC0U) |
                                    ((slot_type[2] >> 2U) & 0x3CU));
}

std::array<uint8_t, 24> sync_dibits(uint64_t pattern)
{
    std::array<uint8_t, 24> result{};
    for (unsigned index = 0; index < result.size(); ++index) {
        result[index] = static_cast<uint8_t>((pattern >> (46U - index * 2U)) & 0x03U);
    }
    return result;
}

std::array<uint8_t, kBurstDibits> make_data_burst(const std::array<uint8_t, 9>& lc,
                                                  unsigned color_code, uint8_t type,
                                                  uint64_t sync_pattern)
{
    std::array<uint8_t, 12> protected_lc{};
    std::copy(lc.begin(), lc.end(), protected_lc.begin());
    const auto rs = rs129(lc);
    const uint8_t mask = type == kVoiceLcHeader ? kVoiceHeaderMask : kTerminatorMask;
    protected_lc[9] = rs[2] ^ mask;
    protected_lc[10] = rs[1] ^ mask;
    protected_lc[11] = rs[0] ^ mask;

    std::array<uint8_t, 33> bytes{};
    bptc19696(protected_lc, bytes);
    add_slot_type(bytes, color_code, type);

    std::array<uint8_t, kBurstDibits> dibits{};
    for (unsigned index = 0; index < bytes.size(); ++index) {
        for (unsigned pair = 0; pair < 4; ++pair) {
            dibits[index * 4U + pair] =
                static_cast<uint8_t>((bytes[index] >> (6U - pair * 2U)) & 0x03U);
        }
    }
    const auto sync = sync_dibits(sync_pattern);
    std::copy(sync.begin(), sync.end(), dibits.begin() + 54);
    return dibits;
}

std::array<bool, 128> make_embedded_lc(const std::array<uint8_t, 9>& lc)
{
    std::array<bool, 72> lc_bits{};
    unsigned checksum = 0;
    for (unsigned index = 0; index < lc.size(); ++index) {
        byte_to_bits(lc[index], lc_bits.data() + index * 8U);
        checksum += lc[index];
    }
    checksum %= 31U;

    std::array<bool, 128> matrix{};
    matrix[106] = (checksum & 0x01U) != 0U;
    matrix[90] = (checksum & 0x02U) != 0U;
    matrix[74] = (checksum & 0x04U) != 0U;
    matrix[58] = (checksum & 0x08U) != 0U;
    matrix[42] = (checksum & 0x10U) != 0U;

    unsigned source = 0;
    constexpr std::array<unsigned, 7> lengths{ 11U, 11U, 10U, 10U, 10U, 10U, 10U };
    for (unsigned row = 0; row < lengths.size(); ++row) {
        for (unsigned column = 0; column < lengths[row]; ++column) {
            matrix[row * 16U + column] = lc_bits[source++];
        }
        hamming_16_11(matrix.data() + row * 16U);
    }
    for (unsigned column = 0; column < 16; ++column) {
        bool column_parity = false;
        for (unsigned row = 0; row < 7; ++row) {
            column_parity ^= matrix[row * 16U + column];
        }
        matrix[112U + column] = column_parity;
    }

    std::array<bool, 128> raw{};
    unsigned matrix_index = 0;
    for (unsigned index = 0; index < raw.size(); ++index) {
        raw[index] = matrix[matrix_index];
        matrix_index += 16U;
        if (matrix_index > 127U) {
            matrix_index -= 127U;
        }
    }
    return raw;
}

uint16_t qr16(unsigned color_code, unsigned lcss)
{
    const uint8_t value = static_cast<uint8_t>((color_code << 3U) | (lcss & 0x03U));
    const uint32_t code15 = (static_cast<uint32_t>(value) << 8U) |
        polynomial_remainder(static_cast<uint32_t>(value) << 8U, 0x139U);
    return static_cast<uint16_t>((code15 << 1U) | (parity(code15) ? 1U : 0U));
}

std::array<uint8_t, 24> make_embedded_signalling(const std::array<bool, 128>& embedded,
                                                 unsigned color_code, unsigned voice_n)
{
    unsigned lcss = 0;
    if (voice_n >= 1U && voice_n <= 4U) {
        lcss = voice_n == 1U ? 1U : (voice_n == 4U ? 2U : 3U);
    }
    const uint16_t emb = qr16(color_code, lcss);
    std::array<bool, 48> bits{};
    for (unsigned index = 0; index < 8; ++index) {
        bits[index] = ((emb >> (15U - index)) & 1U) != 0U;
        bits[40U + index] = ((emb >> (7U - index)) & 1U) != 0U;
    }
    if (voice_n >= 1U && voice_n <= 4U) {
        std::copy_n(embedded.begin() + (voice_n - 1U) * 32U, 32U, bits.begin() + 8U);
    }

    std::array<uint8_t, 24> result{};
    for (unsigned index = 0; index < result.size(); ++index) {
        result[index] = static_cast<uint8_t>((bits[index * 2U] ? 2U : 0U) |
                                             (bits[index * 2U + 1U] ? 1U : 0U));
    }
    return result;
}

class direct_framer_impl final : public direct_framer {
public:
    direct_framer_impl(unsigned source_id, unsigned target_id, unsigned color_code,
                       unsigned slot, unsigned voice_bursts, unsigned idle_frames)
        : gr::block("dmr_direct_framer",
                    gr::io_signature::make(1, 1, kAmbeDibits),
                    gr::io_signature::makev(2, 2, { sizeof(uint8_t), sizeof(float) }))
        , d_slot(slot)
        , d_voice_bursts(voice_bursts)
        , d_idle_frames(idle_frames)
        , d_stage(idle_frames == 0U ? Stage::Header : Stage::Idle)
        , d_lc(make_lc(source_id, target_id))
        , d_embedded(make_embedded_lc(d_lc))
    {
        if (slot != 1U && slot != 2U) {
            throw std::invalid_argument("DMR direct slot must be 1 or 2");
        }
        // This Direct Mode framer configuration uses conventional MS-sourced
        // sync. Slot selection is carried by the 60 ms burst position, not
        // by the Direct Mode sync variants. It has no terminal-device branch;
        // the 828S exercises this behavior on the test bench only.
        constexpr uint64_t data_sync = 0xD5D7F77FD757ULL;
        d_voice_sync = sync_dibits(0x7F7D5DD57DFDULL);
        d_header = make_data_burst(d_lc, color_code, kVoiceLcHeader, data_sync);
        d_terminator = make_data_burst(d_lc, color_code, kTerminatorWithLc, data_sync);
        for (unsigned n = 1; n <= 5; ++n) {
            d_emb_signalling[n - 1U] = make_embedded_signalling(d_embedded, color_code, n);
        }
        set_output_multiple(kFrameDibits);
    }

    void forecast(int, gr_vector_int& required) override
    {
        required[0] = d_stage == Stage::Voice ? 3 : 0;
    }

    int general_work(int noutput_items, gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override
    {
        const auto* input = static_cast<const uint8_t*>(input_items[0]);
        auto* output = static_cast<uint8_t*>(output_items[0]);
        auto* gate = static_cast<float*>(output_items[1]);
        int consumed = 0;
        int produced_frames = 0;
        const int capacity = noutput_items / kFrameDibits;

        while (produced_frames < capacity && d_stage != Stage::Done) {
            if (d_stage == Stage::Idle) {
                std::fill_n(output + produced_frames * kFrameDibits,
                            kFrameDibits, 0U);
                std::fill_n(gate + produced_frames * kFrameDibits,
                            kFrameDibits, 0.0F);
                ++produced_frames;
                if (++d_stage_count == d_idle_frames) {
                    d_stage = Stage::Header;
                    d_stage_count = 0;
                }
                continue;
            }
            std::array<uint8_t, kBurstDibits> burst{};
            if (d_stage == Stage::Header) {
                burst = d_header;
                if (++d_stage_count == kHeaderBursts) {
                    d_stage = Stage::Voice;
                    d_stage_count = 0;
                }
            } else if (d_stage == Stage::Voice) {
                if (d_voice_index >= d_voice_bursts) {
                    d_stage = Stage::Terminator;
                    d_stage_count = 0;
                    continue;
                }
                if (ninput_items[0] - consumed < 3) {
                    break;
                }
                make_voice_burst(input + consumed * kAmbeDibits, d_voice_index % 6U, burst);
                consumed += 3;
                ++d_voice_index;
            } else {
                burst = d_terminator;
                if (++d_stage_count == kTerminatorBursts) {
                    d_stage = Stage::Done;
                }
            }

            emit_frame(burst, output + produced_frames * kFrameDibits,
                       gate + produced_frames * kFrameDibits);
            ++produced_frames;
        }

        consume_each(consumed);
        if (produced_frames == 0 && d_stage == Stage::Done) {
            return WORK_DONE;
        }
        return produced_frames * kFrameDibits;
    }

private:
    enum class Stage { Idle, Header, Voice, Terminator, Done };

    void make_voice_burst(const uint8_t* ambe, unsigned voice_n,
                          std::array<uint8_t, kBurstDibits>& burst) const
    {
        std::copy_n(ambe, 36, burst.begin());
        std::copy_n(ambe + 36, 18, burst.begin() + 36);
        const auto& centre = voice_n == 0U ? d_voice_sync : d_emb_signalling[voice_n - 1U];
        std::copy(centre.begin(), centre.end(), burst.begin() + 54);
        std::copy_n(ambe + 54, 18, burst.begin() + 78);
        std::copy_n(ambe + 72, 36, burst.begin() + 96);
        for (uint8_t& dibit : burst) {
            dibit &= 0x03U;
        }
    }

    void emit_frame(const std::array<uint8_t, kBurstDibits>& burst,
                    uint8_t* output, float* gate) const
    {
        std::fill_n(output, kFrameDibits, 0U);
        std::fill_n(gate, kFrameDibits, 0.0F);
        const unsigned start = d_slot == 1U ? 0U : kSlotDibits;
        std::copy(burst.begin(), burst.end(), output + start);
        std::fill_n(gate + start, kBurstDibits, 1.0F);
    }

    unsigned d_slot;
    unsigned d_voice_bursts;
    unsigned d_idle_frames;
    unsigned d_voice_index = 0;
    unsigned d_stage_count = 0;
    Stage d_stage;
    std::array<uint8_t, 9> d_lc;
    std::array<bool, 128> d_embedded;
    std::array<uint8_t, kBurstDibits> d_header{};
    std::array<uint8_t, kBurstDibits> d_terminator{};
    std::array<uint8_t, 24> d_voice_sync{};
    std::array<std::array<uint8_t, 24>, 5> d_emb_signalling{};
};

} // namespace

DirectFrameBuilder::DirectFrameBuilder(unsigned source_id,
                                       unsigned target_id,
                                       unsigned color_code,
                                       unsigned slot)
    : slot_(slot)
{
    if (source_id == 0U || source_id > 0xFFFFFFU || target_id == 0U ||
        target_id > 0xFFFFFFU || color_code > 15U ||
        (slot_ != 1U && slot_ != 2U)) {
        throw std::invalid_argument("invalid DMR direct frame context");
    }
    constexpr std::uint64_t data_sync = 0xD5D7F77FD757ULL;
    const auto lc = make_lc(source_id, target_id);
    const auto embedded = make_embedded_lc(lc);
    voice_sync_ = sync_dibits(0x7F7D5DD57DFDULL);
    header_ = make_data_burst(lc, color_code, kVoiceLcHeader, data_sync);
    terminator_ = make_data_burst(lc, color_code, kTerminatorWithLc, data_sync);
    for (unsigned index = 1; index <= embedded_signalling_.size(); ++index) {
        embedded_signalling_[index - 1U] =
            make_embedded_signalling(embedded, color_code, index);
    }
}

const DirectFrameBuilder::Burst& DirectFrameBuilder::header_burst() const
{
    return header_;
}

const DirectFrameBuilder::Burst& DirectFrameBuilder::terminator_burst() const
{
    return terminator_;
}

DirectFrameBuilder::Burst DirectFrameBuilder::voice_burst(
    const std::uint8_t* ambe_dibits, unsigned voice_index) const
{
    if (ambe_dibits == nullptr) {
        throw std::invalid_argument("AMBE dibits are null");
    }
    Burst burst{};
    std::copy_n(ambe_dibits, 36, burst.begin());
    std::copy_n(ambe_dibits + 36, 18, burst.begin() + 36);
    const unsigned voice_n = voice_index % 6U;
    const auto& centre = voice_n == 0U
        ? voice_sync_
        : embedded_signalling_[voice_n - 1U];
    std::copy(centre.begin(), centre.end(), burst.begin() + 54);
    std::copy_n(ambe_dibits + 54, 18, burst.begin() + 78);
    std::copy_n(ambe_dibits + 72, 36, burst.begin() + 96);
    for (std::uint8_t& dibit : burst) {
        dibit &= 0x03U;
    }
    return burst;
}

void DirectFrameBuilder::emit_frame(const Burst& burst,
                                    std::uint8_t* output,
                                    float* gate) const
{
    if (output == nullptr || gate == nullptr) {
        throw std::invalid_argument("DMR direct frame output is null");
    }
    std::fill_n(output, kFrameDibits, std::uint8_t{0});
    std::fill_n(gate, kFrameDibits, 0.0F);
    const std::size_t start = slot_ == 1U ? 0U : kSlotDibits;
    std::copy(burst.begin(), burst.end(), output + start);
    std::fill_n(gate + start, kBurstDibits, 1.0F);
}

direct_framer::sptr direct_framer::make(unsigned source_id, unsigned target_id,
                                        unsigned color_code, unsigned slot,
                                        unsigned voice_bursts, unsigned idle_frames)
{
    return gnuradio::make_block_sptr<direct_framer_impl>(
        source_id, target_id, color_code, slot, voice_bursts, idle_frames);
}

} // namespace dmr_b210
