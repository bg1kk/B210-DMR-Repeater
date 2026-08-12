// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
/*
 * gr-dmr Frame Decoder Implementation
 *
 * Copyright (C) 2025 David Kierzkowski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "frame_decoder_impl.h"
#include "core/dmr_log.h"
#include "edac/golay24.h"
#include "bptc19696.h"
#include "trellis.h"
#include <gnuradio/io_signature.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <bitset>
#include <utility>

namespace gr {
namespace dmr {

namespace {

pmt::pmt_t raw_dibits_from_bytes(const std::array<uint8_t, 33>& bytes)
{
    std::array<uint8_t, 132> dibits{};
    for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
        for (std::size_t pair = 0; pair < 4; ++pair) {
            dibits[byte * 4 + pair] = static_cast<uint8_t>(
                (bytes[byte] >> (6U - pair * 2U)) & 0x03U);
        }
    }
    return pmt::init_u8vector(dibits.size(), dibits.data());
}

pmt::pmt_t raw_dibits_from_bits(const std::array<uint8_t, 264>& bits)
{
    std::array<uint8_t, 132> dibits{};
    for (std::size_t index = 0; index < dibits.size(); ++index) {
        dibits[index] = static_cast<uint8_t>(
            ((bits[index * 2] & 1U) << 1U) | (bits[index * 2 + 1] & 1U));
    }
    return pmt::init_u8vector(dibits.size(), dibits.data());
}

uint8_t gf256_multiply(uint8_t left, uint8_t right)
{
    uint8_t product = 0;
    while (right != 0) {
        if ((right & 1U) != 0) {
            product ^= left;
        }
        const bool carry = (left & 0x80U) != 0;
        left <<= 1U;
        if (carry) {
            left ^= 0x1DU;
        }
        right >>= 1U;
    }
    return product;
}

bool rs129_check(const std::array<uint8_t, 12>& payload, uint8_t mask)
{
    // Reed-Solomon (12,9), GF(256) primitive polynomial 0x11D.
    // DMR transmits the three parity bytes in reverse order and masked.
    std::array<uint8_t, 4> parity{};
    constexpr std::array<uint8_t, 3> generator{ 64U, 56U, 14U };

    for (std::size_t i = 0; i < 9; ++i) {
        const uint8_t value = payload[i] ^ parity[2];
        parity[2] = parity[1] ^ gf256_multiply(generator[2], value);
        parity[1] = parity[0] ^ gf256_multiply(generator[1], value);
        parity[0] = gf256_multiply(generator[0], value);
    }

    return static_cast<uint8_t>(payload[9] ^ mask) == parity[2] &&
           static_cast<uint8_t>(payload[10] ^ mask) == parity[1] &&
           static_cast<uint8_t>(payload[11] ^ mask) == parity[0];
}

uint16_t crc16_ccitt(const uint8_t bits[], int length)
{
    uint32_t crc = 0;
    constexpr uint32_t polynomial = 0x1021U;
    for (int index = 0; index < length; ++index) {
        crc <<= 1U;
        crc |= bits[index] & 1U;
        if (crc & 0x10000U) {
            crc = (crc & 0xFFFFU) ^ polynomial;
        }
    }
    return static_cast<uint16_t>((crc ^ 0xFFFFU) & 0xFFFFU);
}

std::string payload_hex(const std::array<uint8_t, 12>& payload)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (uint8_t byte : payload) {
        stream << std::setw(2) << static_cast<unsigned>(byte);
    }
    return stream.str();
}

} // namespace

frame_decoder::sptr frame_decoder::make(float sample_rate, int slot,
                                         int color_code, bool test_mode)
{
    return gnuradio::make_block_sptr<frame_decoder_impl>(
        sample_rate, slot, color_code, test_mode);
}

frame_decoder_impl::frame_decoder_impl(float sample_rate, int slot,
                                        int color_code, bool test_mode)
    : gr::sync_block("dmr_frame_decoder",
                     gr::io_signature::make(1, 1, sizeof(float)),
                     gr::io_signature::make(0, 0, 0))
    , d_sample_rate(sample_rate)
    , d_slot_filter(slot)
    , d_color_code_filter(color_code)
    , d_test_mode(test_mode)
    , d_omega(sample_rate / DMR_SYMBOL_RATE)
    , d_omega_mid(sample_rate / DMR_SYMBOL_RATE)
    , d_omega_rel(0.01f)
    , d_mu(sample_rate / DMR_SYMBOL_RATE)
    , d_gain_mu(0.175f)
    , d_gain_omega(0.25f * 0.175f * 0.175f)
    , d_last_sample(0.0f)
    , d_dl(new float[NUM_FLOAT])
    , d_dl_index(0)
    , d_lock_accum(480)
    , d_lock_threshold(0.2f)
    , d_level_min(-0.5f)
    , d_level_max(0.5f)
    , d_level_alpha(0.02f)
    , d_level_center(0.0f)
    , d_sync_register(0)
    , d_sync_found(false)
    , d_current_sync(SyncPattern::UNKNOWN)
    , d_sync_errors(0)
    , d_state(State::SEARCHING)
    , d_bits_collected(0)
    , d_sync_count(0)
    , d_frame_count(0)
    , d_total_samples(0)
    , d_samples_since_sync(0)
    , d_current_slot(Slot::SLOT1)
    , d_last_frame_symbols(0)
    , d_voice_state(VoiceState::IDLE)
    , d_voice_sync_type(0)
    , d_voice_frame_index(0)
    , d_next_frame_symbols(0)
    , d_symbol_count(0)
    , d_dmr_slot(0, 0)
    , d_test_running(false)
{
    d_port_out = pmt::mp("frames");
    message_port_register_out(d_port_out);

    d_frame_bits.fill(0);

    d_min_omega = d_omega * (1.0f - d_omega_rel);
    d_max_omega = d_omega * (1.0f + d_omega_rel);
    d_twice_sps = 2 * (int)std::ceil(d_omega);
    std::memset(d_dl, 0, NUM_FLOAT * sizeof(float));

    d_dmr_slot.set_lc_callback([this](int slot, uint8_t cc, const dmr_lc_data& lc, bool is_terminator) {
        on_embedded_lc_complete(lc);
    });

    if (d_test_mode) {
        d_test_running = true;
        d_test_thread = std::thread(&frame_decoder_impl::test_thread_func, this);
    }

    DMRLog::log(LogCategory::STATS, 0.0f)
        << "Init rate=" << sample_rate
        << " sps=" << d_omega
        << " slot=" << slot
        << " cc=" << color_code
        << std::endl;
}

frame_decoder_impl::~frame_decoder_impl()
{
    if (d_test_running) {
        d_test_running = false;
        if (d_test_thread.joinable()) {
            d_test_thread.join();
        }
    }
    delete[] d_dl;
}

int frame_decoder_impl::popcount64(uint64_t x)
{
    return __builtin_popcountll(x);
}

SyncPattern frame_decoder_impl::match_sync(uint64_t pattern, int& errors)
{
    const struct {
        uint64_t pattern;
        SyncPattern type;
    } sync_table[] = {
        { SYNC_BS_DATA,     SyncPattern::BS_DATA_SYNC },
        { SYNC_BS_VOICE,    SyncPattern::BS_VOICE_SYNC },
        { SYNC_MS_DATA,     SyncPattern::MS_DATA_SYNC },
        { SYNC_MS_VOICE,    SyncPattern::MS_VOICE_SYNC },
        { SYNC_MS_REVERSE,  SyncPattern::MS_REVERSE_SYNC },
        { SYNC_DIRECT_DATA_TS1,  SyncPattern::DIRECT_DATA_TS1 },
        { SYNC_DIRECT_DATA_TS2,  SyncPattern::DIRECT_DATA_TS2 },
        { SYNC_DIRECT_VOICE_TS1, SyncPattern::DIRECT_VOICE_TS1 },
        { SYNC_DIRECT_VOICE_TS2, SyncPattern::DIRECT_VOICE_TS2 },
    };

    int best_errors = 49;  // More than 48 bits
    SyncPattern best_match = SyncPattern::UNKNOWN;

    for (const auto& entry : sync_table) {
        int err = popcount64(pattern ^ entry.pattern);
        if (err < best_errors) {
            best_errors = err;
            best_match = entry.type;
        }
    }

    errors = best_errors;
    // The upstream B210 burst sampler has already isolated and aligned a DMR
    // burst. Keep the same six-bit RF tolerance here so LC metadata is not
    // discarded after the raw burst has been accepted.
    constexpr int reliable_sync_error_limit = 6;
    return (best_errors <= reliable_sync_error_limit) ? best_match : SyncPattern::UNKNOWN;
}

int frame_decoder_impl::find_best_sync_error()
{
    int best = 49;
    for (int i = 0; i < NUM_SYNC_PATTERNS; i++) {
        int err = popcount64(d_sync_register ^ SYNC_PATTERNS[i]);
        if (err < best) best = err;
    }
    return best;
}

Slot frame_decoder_impl::determine_slot(SyncPattern sync)
{
    switch (sync) {
        case SyncPattern::DIRECT_DATA_TS1:
        case SyncPattern::DIRECT_VOICE_TS1:
            return Slot::SLOT1;

        case SyncPattern::DIRECT_DATA_TS2:
        case SyncPattern::DIRECT_VOICE_TS2:
            return Slot::SLOT2;

        case SyncPattern::BS_DATA_SYNC:
        case SyncPattern::BS_VOICE_SYNC:
        case SyncPattern::MS_DATA_SYNC:
        case SyncPattern::MS_VOICE_SYNC:
        case SyncPattern::MS_REVERSE_SYNC:
            {
                uint64_t symbols_since_last = d_symbol_count - d_last_frame_symbols;

                if (d_last_frame_symbols == 0 || symbols_since_last > 400) {
                    d_current_slot = Slot::SLOT1;
                } else if (symbols_since_last > 100 && symbols_since_last < 200) {
                    d_current_slot = (d_current_slot == Slot::SLOT1) ? Slot::SLOT2 : Slot::SLOT1;
                }

                d_last_frame_symbols = d_symbol_count;
                return d_current_slot;
            }

        default:
            return Slot::UNKNOWN;
    }
}

uint8_t frame_decoder_impl::quantize_symbol(float sample)
{
    float center_alpha = (d_level_max < 1.0f) ? 0.01f : 0.001f;
    d_level_center = d_level_center + center_alpha * (sample - d_level_center);

    float deviation = std::abs(sample - d_level_center);

    float expected_deviation = (d_level_max - d_level_min) / 2.0f;
    if (deviation > expected_deviation) {
        d_level_max = d_level_max + 0.1f * (deviation - expected_deviation);
    } else {
        d_level_max = d_level_max + d_level_alpha * (deviation - expected_deviation);
    }

    d_level_min = -d_level_max;

    float dev = d_level_max;
    if (dev < 0.05f) dev = 0.5f;

    float thresh_high = d_level_center + dev * 0.67f;
    float thresh_mid = d_level_center;
    float thresh_low = d_level_center - dev * 0.67f;

    if (sample > thresh_high) {
        return 0b01;
    } else if (sample > thresh_mid) {
        return 0b00;
    } else if (sample > thresh_low) {
        return 0b10;
    } else {
        return 0b11;
    }
}

void frame_decoder_impl::add_dibit(uint8_t dibit)
{
    d_symbol_count++;

    d_dibit_buffer.push_back(dibit);
    if (d_dibit_buffer.size() > DIBIT_BUFFER_SIZE) {
        d_dibit_buffer.pop_front();
    }

    d_sync_register = ((d_sync_register << 2) | dibit) & 0xFFFFFFFFFFFFULL;

    if (d_voice_state == VoiceState::IN_SUPERFRAME) {
        check_voice_frame_timing();
    }

    if (d_state == State::SEARCHING) {
        const float MIN_SIGNAL_DEVIATION = 0.0f;

        if (d_level_max >= MIN_SIGNAL_DEVIATION) {
            int errors;
            SyncPattern match = match_sync(d_sync_register, errors);

            if (match != SyncPattern::UNKNOWN) {
                d_sync_found = true;
                d_current_sync = match;
                d_sync_errors = errors;
                d_sync_count++;
                d_samples_since_sync = 0;

                float time_sec = (float)d_total_samples / d_sample_rate;
                DMRLog::log(LogCategory::SYNC, time_sec)
                    << syncPatternToString(match) << " err=" << errors << std::endl;

                if (isVoiceSync(match)) {
                    enter_voice_superframe(syncPatternToMagic(match));
                }

                d_state = State::COLLECTING;
                d_bits_collected = 0;
                d_frame_bits.fill(0);
            }
        }
    } else if (d_state == State::COLLECTING) {
        if (d_bits_collected < 108) {
            int bit_pos = 156 + d_bits_collected;
            d_frame_bits[bit_pos] = (dibit >> 1) & 1;
            d_frame_bits[bit_pos + 1] = dibit & 1;
            d_bits_collected += 2;

            if (d_bits_collected >= 108) {
                size_t buf_size = d_dibit_buffer.size();
                if (buf_size >= 132) {
                    for (int i = 0; i < 54; i++) {
                        size_t idx = buf_size - 132 + i;
                        uint8_t db = d_dibit_buffer[idx];
                        d_frame_bits[i * 2] = (db >> 1) & 1;
                        d_frame_bits[i * 2 + 1] = db & 1;
                    }

                    for (int i = 0; i < 24; i++) {
                        size_t idx = buf_size - 78 + i;
                        uint8_t db = d_dibit_buffer[idx];
                        d_frame_bits[108 + i * 2] = (db >> 1) & 1;
                        d_frame_bits[108 + i * 2 + 1] = db & 1;
                    }
                }

                d_state = State::DECODING;
                decode_frame();
                d_state = State::SEARCHING;
            }
        }
    }
}

void frame_decoder_impl::decode_frame()
{
    DMRBurst burst;
    burst.sync_pattern = d_current_sync;
    burst.sync_errors = d_sync_errors;
    burst.sync_valid = true;
    burst.slot = determine_slot(d_current_sync);

    if (d_slot_filter > 0 && static_cast<int>(burst.slot) != d_slot_filter) {
        return;
    }

    for (int i = 0; i < 264; i += 8) {
        uint8_t byte = 0;
        for (int j = 0; j < 8 && (i + j) < 264; j++) {
            byte = (byte << 1) | d_frame_bits[i + j];
        }
        burst.raw_bytes[i / 8] = byte;
    }

    if (isVoiceSync(burst.sync_pattern)) {
        d_frame_count++;

        float time_sec = (float)d_total_samples / d_sample_rate;
        DMRLog::log(LogCategory::FRAME, time_sec)
            << syncPatternToString(burst.sync_pattern) << " VOICE" << std::endl;

        publish_burst(burst);
        return;
    }

    if (!decode_slot_type(burst)) {
        float time_sec = (float)d_total_samples / d_sample_rate;
        DMRLog::log(LogCategory::ERROR, time_sec) << "Slot type decode failed" << std::endl;
        return;
    }

    if (d_color_code_filter >= 0 && burst.color_code != d_color_code_filter) {
        return;
    }

    if (isDataSync(burst.sync_pattern)) {
        if (burst.data_type == DataType::VOICE_LC_HEADER ||
            burst.data_type == DataType::TERMINATOR_LC ||
            burst.data_type == DataType::CSBK ||
            burst.data_type == DataType::DATA_HEADER ||
            burst.data_type == DataType::RATE_1_2_DATA) {
            decode_payload(burst);
        } else if (burst.data_type == DataType::RATE_3_4_DATA) {
            decode_trellis_data(burst);
        }
    }

    d_frame_count++;

    if (burst.data_type == DataType::IDLE) {
        publish_burst(burst);
        return;
    }

    float time_sec = (float)d_total_samples / d_sample_rate;

    auto& out = DMRLog::log(LogCategory::FRAME, time_sec);
    out << syncPatternToString(burst.sync_pattern)
        << " S" << static_cast<int>(burst.slot)
        << " CC=" << (int)burst.color_code
        << " " << DMRLog::dataTypeShort((uint8_t)burst.data_type);

    if (burst.lc_valid) {
        out << " src=" << burst.source_id
            << " dst=" << burst.dest_id
            << (burst.call_type == CallType::GROUP_CALL ? " GRP" : " PVT");
        if (burst.emergency) out << " EMERG";
    }

    if (burst.data_type == DataType::CSBK) {
        out << " op=0x" << std::hex << (int)burst.csbk_opcode << std::dec;
        if (burst.csbk_last_block) out << " LB";
    }

    if (isDataSync(burst.sync_pattern) &&
        burst.data_type != DataType::IDLE) {
        out << " valid=" << (burst.data_valid ? "yes" : "no");
    }

    out << std::endl;

    publish_burst(burst);
}

bool frame_decoder_impl::decode_slot_type(DMRBurst& burst)
{
    uint32_t slot_type_bits = 0;

    for (int i = 0; i < 20; i++) {
        int idx = SLOT_TYPE_INDICES_264[i];
        if (idx < 264) {
            slot_type_bits = (slot_type_bits << 1) | d_frame_bits[idx];
        }
    }

    float time_sec = (float)d_total_samples / d_sample_rate;
    DMRLog::log(LogCategory::DECODE, time_sec)
        << "SlotType raw=0x" << std::hex << slot_type_bits << std::dec << std::endl;

    uint32_t codeword = slot_type_bits;

    int result = edac::Golay24::checkAndCorrect(codeword);

    if (result == 2) {
        burst.color_code = (slot_type_bits >> 16) & 0x0F;
        burst.data_type = static_cast<DataType>((slot_type_bits >> 12) & 0x0F);
        burst.slot_type_valid = false;

        DMRLog::log(LogCategory::DECODE, time_sec)
            << "Golay FAIL CC=" << (int)burst.color_code
            << " DT=" << (int)burst.data_type << std::endl;
        return false;
    }

    burst.color_code = (codeword >> 16) & 0x0F;
    burst.data_type = static_cast<DataType>((codeword >> 12) & 0x0F);
    burst.slot_type_valid = true;

    DMRLog::log(LogCategory::DECODE, time_sec)
        << "Golay OK CC=" << (int)burst.color_code
        << " DT=" << DMRLog::dataTypeShort((uint8_t)burst.data_type) << std::endl;

    return true;
}

bool frame_decoder_impl::decode_payload(DMRBurst& burst)
{
    const float time_sec = static_cast<float>(d_total_samples) / d_sample_rate;
    uint8_t extracted[96]{};
    CBPTC19696 bptc;
    const bool bptc_valid = bptc.decode(d_frame_bits.data(), extracted);
    burst.fec_valid = bptc_valid;
    if (!bptc_valid && (burst.sync_errors > 1 || !burst.slot_type_valid)) {
        burst.lc_valid = false;
        return false;
    }

    for (int i = 0; i < 12; i++) {
        uint8_t byte = 0;
        for (int j = 0; j < 8; j++) {
            byte = (byte << 1) | extracted[i * 8 + j];
        }
        burst.payload[i] = byte;
    }

    if (burst.data_type == DataType::VOICE_LC_HEADER ||
        burst.data_type == DataType::TERMINATOR_LC) {

        const uint8_t parity_mask = burst.data_type == DataType::VOICE_LC_HEADER
            ? 0x96U
            : 0x99U;
        if (!rs129_check(burst.payload, parity_mask)) {
            burst.lc_valid = false;
            return false;
        }

        burst.dest_id = (burst.payload[3] << 16) |
                        (burst.payload[4] << 8) |
                        burst.payload[5];

        burst.source_id = (burst.payload[6] << 16) |
                          (burst.payload[7] << 8) |
                          burst.payload[8];

        const uint8_t opcode = burst.payload[0] & 0x3F;
        burst.call_type = (opcode == 0x03) ? CallType::PRIVATE_CALL : CallType::GROUP_CALL;

        burst.emergency = (burst.payload[2] & 0x80) != 0;

        burst.lc_valid = true;

        if (burst.data_type == DataType::VOICE_LC_HEADER) {
            d_dmr_slot.set_cc(burst.color_code);
        }
    } else if (burst.data_type == DataType::CSBK) {
        std::array<uint8_t, 96> crc_bits{};
        std::copy_n(extracted, crc_bits.size(), crc_bits.begin());
        constexpr uint16_t crc_mask = 0xA5A5U;
        for (int bit = 0; bit < 16; ++bit) {
            crc_bits[80 + bit] ^= static_cast<uint8_t>(
                (crc_mask >> (15 - bit)) & 1U);
        }
        const bool crc_valid =
            crc16_ccitt(crc_bits.data(), static_cast<int>(crc_bits.size())) == 0;
        burst.data_valid = bptc_valid && crc_valid;
        DMRLog::log(LogCategory::DECODE, time_sec)
            << "CSBK BPTC=" << (bptc_valid ? "OK" : "FAIL")
            << " CRC=" << (crc_valid ? "OK" : "FAIL")
            << " payload=" << payload_hex(burst.payload) << std::endl;
        burst.csbk_last_block = (burst.payload[0] & 0x80) != 0;
        burst.csbk_opcode = burst.payload[0] & 0x3F;

        switch (burst.csbk_opcode) {
            case 0x04:
            case 0x38:
            case 0x3D:
            case 0x3E:
            case 0x30:
            case 0x31:
                if (burst.csbk_opcode == 0x30 || burst.csbk_opcode == 0x31 ||
                    burst.csbk_opcode == 0x3D || burst.csbk_opcode == 0x3E) {
                    burst.dest_id = (burst.payload[4] << 16) |
                                    (burst.payload[5] << 8) |
                                    burst.payload[6];
                    burst.source_id = (burst.payload[7] << 16) |
                                      (burst.payload[8] << 8) |
                                      burst.payload[9];
                    burst.call_type = (burst.csbk_opcode == 0x30 || burst.csbk_opcode == 0x3E)
                                      ? CallType::PRIVATE_CALL : CallType::GROUP_CALL;
                    burst.lc_valid = true;
                }
                break;
            default:
                break;
        }
    } else if (burst.data_type == DataType::DATA_HEADER) {
        constexpr uint16_t crc_mask = 0xCCCCU;
        for (int bit = 0; bit < 16; ++bit) {
            extracted[80 + bit] ^= static_cast<uint8_t>(
                (crc_mask >> (15 - bit)) & 1U);
        }
        const bool crc_valid = crc16_ccitt(extracted, 96) == 0;
        // Header CRC authenticates the recovered header even when the BPTC
        // decoder reports an unresolved correction cycle.
        burst.data_valid = crc_valid;
        DMRLog::log(LogCategory::DECODE, time_sec)
            << "DATA-H BPTC=" << (bptc_valid ? "OK" : "FAIL")
            << " CRC=" << (crc_valid ? "OK" : "FAIL")
            << " payload=" << payload_hex(burst.payload) << std::endl;
    } else if (burst.data_type == DataType::RATE_1_2_DATA) {
        // Unconfirmed R1/2 blocks have no per-block CRC. Their integrity is
        // established by the mandatory CRC32 on the complete data packet.
        burst.data_valid = true;
        DMRLog::log(LogCategory::DECODE, time_sec)
            << "R1/2 BPTC=" << (bptc_valid ? "OK" : "FAIL")
            << " payload=" << payload_hex(burst.payload) << std::endl;
    }

    return true;
}

bool frame_decoder_impl::decode_trellis_data(DMRBurst& burst)
{
    std::array<unsigned char, 18> payload{};
    CDMRTrellis trellis;
    burst.data_valid = trellis.decode(d_frame_bits.data(), payload.data());
    return burst.data_valid;
}

pmt::pmt_t frame_decoder_impl::burst_to_pmt(const DMRBurst& burst)
{
    pmt::pmt_t dict = pmt::make_dict();

    const bool voice_sync = isVoiceSync(burst.sync_pattern);
    const std::string data_type = voice_sync && burst.data_type == DataType::UNKNOWN
        ? "VOICE_A"
        : dataTypeToString(burst.data_type);

    dict = pmt::dict_add(dict, pmt::mp("color_code"),
                         pmt::from_long(burst.color_code));
    dict = pmt::dict_add(dict, pmt::mp("slot"),
                         pmt::from_long(static_cast<int>(burst.slot)));
    dict = pmt::dict_add(dict, pmt::mp("data_type"),
                         pmt::from_long(static_cast<int>(burst.data_type)));
    dict = pmt::dict_add(dict, pmt::mp("data_type_str"),
                         pmt::intern(data_type));
    dict = pmt::dict_add(dict, pmt::mp("sync_pattern"),
                         pmt::intern(syncPatternToString(burst.sync_pattern)));
    dict = pmt::dict_add(dict, pmt::mp("sync_errors"),
                         pmt::from_long(burst.sync_errors));
    dict = pmt::dict_add(dict, pmt::mp("sync_valid"),
                         pmt::from_bool(burst.sync_valid));
    dict = pmt::dict_add(dict, pmt::mp("slot_type_valid"),
                         pmt::from_bool(burst.slot_type_valid));
    dict = pmt::dict_add(dict, pmt::mp("lc_valid"),
                         pmt::from_bool(burst.lc_valid));
    dict = pmt::dict_add(dict, pmt::mp("data_valid"),
                         pmt::from_bool(burst.data_valid));
    dict = pmt::dict_add(dict, pmt::mp("fec_valid"),
                         pmt::from_bool(burst.fec_valid));
    dict = pmt::dict_add(dict, pmt::mp("raw_dibits"),
                         raw_dibits_from_bytes(burst.raw_bytes));

    if (burst.data_type == DataType::VOICE_LC_HEADER ||
        burst.data_type == DataType::TERMINATOR_LC ||
        burst.data_type == DataType::CSBK ||
        burst.data_type == DataType::DATA_HEADER ||
        burst.data_type == DataType::RATE_1_2_DATA) {
        dict = pmt::dict_add(dict, pmt::mp("payload"),
                             pmt::init_u8vector(burst.payload.size(),
                                                burst.payload.data()));
    }

    if (burst.lc_valid) {
        dict = pmt::dict_add(dict, pmt::mp("source_id"),
                             pmt::from_long(burst.source_id));
        dict = pmt::dict_add(dict, pmt::mp("dest_id"),
                             pmt::from_long(burst.dest_id));
        dict = pmt::dict_add(dict, pmt::mp("call_type"),
                             pmt::from_long(static_cast<int>(burst.call_type)));
        dict = pmt::dict_add(dict, pmt::mp("emergency"),
                             pmt::from_bool(burst.emergency));
    }

    if (burst.data_type == DataType::CSBK) {
        dict = pmt::dict_add(dict, pmt::mp("csbk_opcode"),
                             pmt::from_long(burst.csbk_opcode));
        dict = pmt::dict_add(dict, pmt::mp("csbk_last_block"),
                             pmt::from_bool(burst.csbk_last_block));
    }

    return dict;
}

void frame_decoder_impl::publish_burst(const DMRBurst& burst)
{
    pmt::pmt_t msg = burst_to_pmt(burst);
    message_port_pub(d_port_out, msg);
}

void frame_decoder_impl::process_sample(float sample)
{
    d_dl[d_dl_index] = sample;
    d_dl_index = (d_dl_index + 1) % d_twice_sps;

    d_mu -= 1.0f;

    if (d_mu <= 0.0f) {
        d_mu += d_omega;

        uint8_t dibit = quantize_symbol(sample);

        add_dibit(dibit);

        float level = std::abs(sample - d_level_center);
        d_lock_accum.add(level > 0.1f ? 1.0f : 0.0f);
    }
}

int frame_decoder_impl::work(int noutput_items,
                              gr_vector_const_void_star& input_items,
                              gr_vector_void_star& output_items)
{
    (void)output_items;
    const float* in = (const float*)input_items[0];

    static uint64_t last_print = 0;
    static float sample_min = 1e9f, sample_max = -1e9f;
    static double sample_sum = 0;
    static uint64_t sample_count = 0;
    static uint64_t dibit_counts[4] = {0, 0, 0, 0};
    static int min_sync_err_period = 49;
    static float max_dev_period = 0.0f;

    for (int i = 0; i < noutput_items; i++) {
        float sample = in[i];

        if (sample < sample_min) sample_min = sample;
        if (sample > sample_max) sample_max = sample;
        sample_sum += sample;
        sample_count++;

        process_sample(sample);

        int curr_err = find_best_sync_error();
        if (curr_err < min_sync_err_period) min_sync_err_period = curr_err;
        if (d_level_max > max_dev_period) max_dev_period = d_level_max;
    }

    d_total_samples += noutput_items;
    d_samples_since_sync += noutput_items;

    uint64_t silence_threshold = (uint64_t)(SILENCE_RESET_SECONDS * d_sample_rate);
    if (d_samples_since_sync >= silence_threshold) {
        float time_sec = (float)d_total_samples / d_sample_rate;
        DMRLog::log(LogCategory::SYNC, time_sec)
            << "No sync for " << SILENCE_RESET_SECONDS << "s - resetting" << std::endl;

        d_mu = d_omega_mid;
        d_omega = d_omega_mid;
        d_last_sample = 0.0f;
        d_lock_accum.reset();
        std::memset(d_dl, 0, NUM_FLOAT * sizeof(float));
        d_dl_index = 0;

        d_level_center = 0.0f;
        d_level_min = -0.5f;
        d_level_max = 0.5f;

        d_sync_register = 0;
        d_sync_found = false;
        d_current_sync = SyncPattern::UNKNOWN;
        d_state = State::SEARCHING;
        d_dibit_buffer.clear();

        d_samples_since_sync = 0;
    }

    if (d_total_samples - last_print >= 96000) {
        dibit_counts[0] = dibit_counts[1] = dibit_counts[2] = dibit_counts[3] = 0;
        for (size_t i = 0; i < d_dibit_buffer.size(); i++) {
            uint8_t db = d_dibit_buffer[i];
            if (db < 4) dibit_counts[db]++;
        }

        float stats_time = (float)d_total_samples / d_sample_rate;
        bool locked = d_lock_accum.avg() >= d_lock_threshold;
        DMRLog::log(LogCategory::STATS, stats_time)
            << "syncs=" << d_sync_count.load()
            << " frames=" << d_frame_count.load()
            << " lock=" << (locked ? "YES" : "no")
            << " q=" << std::fixed << std::setprecision(2) << d_lock_accum.avg()
            << " omega=" << std::setprecision(3) << d_omega
            << " ctr=" << std::setprecision(2) << d_level_center
            << " dev=" << d_level_max
            << " max_dev=" << max_dev_period
            << " min_err=" << min_sync_err_period
            << std::endl;

        min_sync_err_period = 49;
        max_dev_period = 0.0f;

        sample_min = 1e9f;
        sample_max = -1e9f;
        sample_sum = 0;
        sample_count = 0;
        last_print = d_total_samples;
    }

    return noutput_items;
}

void frame_decoder_impl::test_thread_func()
{
    while (d_test_running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (d_test_running) {
            generate_test_frame();
        }
    }
}

void frame_decoder_impl::generate_test_frame()
{
    static int test_counter = 0;

    DMRBurst burst;
    burst.sync_pattern = SyncPattern::BS_DATA_SYNC;
    burst.sync_errors = 0;
    burst.sync_valid = true;
    burst.color_code = 1;
    burst.data_type = DataType::VOICE_LC_HEADER;
    burst.slot_type_valid = true;
    burst.slot = Slot::SLOT1;
    burst.source_id = 1234567 + test_counter;
    burst.dest_id = 100 + (test_counter % 10);
    burst.call_type = CallType::GROUP_CALL;
    burst.emergency = (test_counter % 5 == 0);
    burst.lc_valid = true;

    test_counter++;
    d_frame_count++;

    float time_sec = (float)d_total_samples / d_sample_rate;
    DMRLog::log(LogCategory::FRAME, time_sec)
        << "TEST src=" << burst.source_id << " dst=" << burst.dest_id << std::endl;

    publish_burst(burst);
}

void frame_decoder_impl::set_slot(int slot)
{
    d_slot_filter = slot;
}

void frame_decoder_impl::set_color_code(int cc)
{
    d_color_code_filter = cc;
}

uint64_t frame_decoder_impl::get_sync_count() const
{
    return d_sync_count;
}

uint64_t frame_decoder_impl::get_frame_count() const
{
    return d_frame_count;
}

void frame_decoder_impl::reset()
{
    d_sync_register = 0;
    d_sync_found = false;
    d_current_sync = SyncPattern::UNKNOWN;
    d_state = State::SEARCHING;
    d_bits_collected = 0;
    d_dibit_buffer.clear();
    d_sync_count = 0;
    d_frame_count = 0;
    d_total_samples = 0;
    d_samples_since_sync = 0;
    d_level_center = 0.0f;
    d_level_min = -0.5f;
    d_level_max = 0.5f;

    d_mu = d_omega_mid;
    d_omega = d_omega_mid;
    d_last_sample = 0.0f;
    d_lock_accum.reset();
    std::memset(d_dl, 0, NUM_FLOAT * sizeof(float));
    d_dl_index = 0;

    d_voice_state = VoiceState::IDLE;
    d_voice_sync_type = 0;
    d_voice_frame_index = 0;
    d_next_frame_symbols = 0;
    d_symbol_count = 0;
}

void frame_decoder_impl::set_debug(bool enable)
{
    DMRLog::enabled = enable;
}

void frame_decoder_impl::enter_voice_superframe(uint64_t sync_type)
{
    d_voice_state = VoiceState::IN_SUPERFRAME;
    d_voice_sync_type = sync_type;
    d_voice_frame_index = 0;

    d_next_frame_symbols = d_symbol_count + (FRAME_SYMBOLS * 2);

    float time_sec = (float)d_total_samples / d_sample_rate;
    DMRLog::log(LogCategory::DECODE, time_sec)
        << "Voice superframe start, next frame at symbol " << d_next_frame_symbols << std::endl;
}

void frame_decoder_impl::check_voice_frame_timing()
{
    if (d_symbol_count >= d_next_frame_symbols - FRAME_SYMBOL_TOLERANCE &&
        d_symbol_count <= d_next_frame_symbols + FRAME_SYMBOL_TOLERANCE) {

        d_voice_frame_index++;

        if (d_voice_frame_index >= SUPERFRAME_FRAMES) {
            d_voice_state = VoiceState::IDLE;
            return;
        }

        collect_voice_frame();

        d_next_frame_symbols = d_symbol_count + (FRAME_SYMBOLS * 2);
    }

    if (d_symbol_count > d_next_frame_symbols + (FRAME_SYMBOLS / 2)) {
        float time_sec = (float)d_total_samples / d_sample_rate;
        DMRLog::log(LogCategory::DECODE, time_sec)
            << "Voice superframe timing lost at frame " << d_voice_frame_index << std::endl;
        d_voice_state = VoiceState::IDLE;
    }
}

void frame_decoder_impl::collect_voice_frame()
{
    size_t buf_size = d_dibit_buffer.size();
    if (buf_size < 132) {
        return;
    }

    std::array<uint8_t, 264> frame_bits;
    frame_bits.fill(0);

    for (int i = 0; i < 132; i++) {
        size_t idx = buf_size - 132 + i;
        if (idx < buf_size) {
            uint8_t db = d_dibit_buffer[idx];
            frame_bits[i * 2] = (db >> 1) & 1;
            frame_bits[i * 2 + 1] = db & 1;
        }
    }

    decode_embedded_signaling();

    float time_sec = (float)d_total_samples / d_sample_rate;
    char frame_letter = 'A' + d_voice_frame_index;
    DMRLog::log(LogCategory::FRAME, time_sec)
        << "VOICE_" << frame_letter
        << " (embedded)" << std::endl;

    DMRBurst burst;
    burst.sample_index = d_total_samples;
    burst.timestamp_us = (uint64_t)(time_sec * 1e6);
    burst.sync_pattern = static_cast<SyncPattern>(d_voice_sync_type);
    burst.sync_errors = 0;
    burst.sync_valid = false;
    burst.slot_type_valid = false;
    burst.lc_valid = false;

    d_frame_count++;

    pmt::pmt_t dict = pmt::make_dict();
    dict = pmt::dict_add(dict, pmt::mp("color_code"), pmt::from_long(d_dmr_slot.get_cc()));
    dict = pmt::dict_add(dict, pmt::mp("slot"), pmt::from_long(static_cast<int>(d_current_slot)));
    dict = pmt::dict_add(dict, pmt::mp("data_type"), pmt::from_long(255));
    std::string frame_name = std::string("VOICE_") + frame_letter;
    dict = pmt::dict_add(dict, pmt::mp("data_type_str"), pmt::intern(frame_name));
    dict = pmt::dict_add(dict, pmt::mp("sync_pattern"), pmt::intern(syncPatternToString(burst.sync_pattern)));
    dict = pmt::dict_add(dict, pmt::mp("sync_errors"), pmt::from_long(0));
    dict = pmt::dict_add(dict, pmt::mp("voice_frame"), pmt::from_long(d_voice_frame_index));
    dict = pmt::dict_add(dict, pmt::mp("sync_valid"), pmt::from_bool(true));
    dict = pmt::dict_add(dict, pmt::mp("lc_valid"), pmt::from_bool(false));
    dict = pmt::dict_add(dict, pmt::mp("raw_dibits"),
                         raw_dibits_from_bits(frame_bits));

    message_port_pub(d_port_out, dict);
}

void frame_decoder_impl::decode_embedded_signaling()
{
    size_t buf_size = d_dibit_buffer.size();
    if (buf_size < 132) {
        return;
    }

    uint8_t slot_bits[264];
    memset(slot_bits, 0, sizeof(slot_bits));

    for (int i = 0; i < 132; i++) {
        size_t idx = buf_size - 132 + i;
        if (idx < buf_size) {
            uint8_t db = d_dibit_buffer[idx];
            slot_bits[i * 2] = (db >> 1) & 1;
            slot_bits[i * 2 + 1] = db & 1;
        }
    }

    d_dmr_slot.load_slot(slot_bits, 0);
}

void frame_decoder_impl::on_embedded_lc_complete(const dmr_lc_data& lc)
{
    if (!lc.valid) {
        return;
    }

    float time_sec = (float)d_total_samples / d_sample_rate;
    DMRLog::log(LogCategory::FRAME, time_sec)
        << "EMB_LC src=" << lc.srcaddr
        << " dst=" << lc.dstaddr
        << " FLCO=" << std::hex << (int)lc.flco << std::dec
        << std::endl;

    DMRBurst burst;
    burst.sample_index = d_total_samples;
    burst.timestamp_us = (uint64_t)(time_sec * 1e6);
    burst.sync_pattern = static_cast<SyncPattern>(d_voice_sync_type);
    burst.sync_errors = 0;
    burst.sync_valid = false;
    burst.color_code = d_dmr_slot.get_cc();
    burst.data_type = DataType::EMBEDDED_LC;
    burst.slot_type_valid = false;

    burst.source_id = lc.srcaddr;
    burst.dest_id = lc.dstaddr;
    burst.call_type = (lc.flco & 0x01) ? CallType::PRIVATE_CALL : CallType::GROUP_CALL;
    burst.emergency = (lc.svcopt & 0x80) != 0;
    burst.lc_valid = true;

    publish_burst(burst);
}

} // namespace dmr
} // namespace gr
