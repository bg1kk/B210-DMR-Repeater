// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dmr_rpt {

struct ShortMessageHeader {
    bool group = false;
    bool response_requested = false;
    std::uint8_t appended_blocks = 0;
    std::uint8_t sap = 0;
    std::uint32_t destination_id = 0;
    std::uint32_t source_id = 0;
    std::uint8_t defined_data_format = 0;
    bool sarq = false;
    bool full_message = false;
    std::uint8_t pad_octets = 0;
};

struct ShortMessagePreamble {
    bool group = false;
    std::uint32_t destination_id = 0;
    std::uint32_t source_id = 0;
};

struct ShortMessagePdu {
    std::array<std::uint8_t, 12> header{};
    std::vector<std::array<std::uint8_t, 12>> data_blocks;
};

struct DecodedShortMessage {
    ShortMessageHeader header;
    std::vector<std::uint8_t> user_data;
    std::string text_utf8;
};

bool parse_defined_short_message_header(
    const std::array<std::uint8_t, 12>& payload,
    ShortMessageHeader& header,
    std::string& error);

bool decode_unconfirmed_rate_half_short_message(
    const ShortMessageHeader& header,
    const std::vector<std::array<std::uint8_t, 12>>& data_blocks,
    DecodedShortMessage& message,
    std::string& error);

bool build_unconfirmed_utf16be_short_message(
    std::uint32_t source_id,
    std::uint32_t destination_id,
    bool group,
    const std::string& text_utf8,
    ShortMessagePdu& message,
    std::string& error);

std::array<std::uint8_t, 12> build_828s_data_preamble(
    std::uint32_t source_id,
    std::uint32_t destination_id,
    bool group);

bool parse_828s_data_preamble(
    const std::array<std::uint8_t, 12>& payload,
    ShortMessagePreamble& preamble,
    std::string& error);

bool build_828s_short_message_header(
    const ShortMessagePreamble& preamble,
    std::uint8_t appended_blocks,
    std::array<std::uint8_t, 12>& header,
    std::string& error);

bool parse_828s_short_message_header(
    const std::array<std::uint8_t, 12>& payload,
    ShortMessageHeader& header,
    std::string& error);

bool decode_828s_short_message(
    const ShortMessageHeader& header,
    const std::vector<std::array<std::uint8_t, 12>>& data_blocks,
    DecodedShortMessage& message,
    std::string& error);

bool recover_828s_utf16le_short_message(
    const ShortMessageHeader& header,
    const std::vector<std::array<std::uint8_t, 12>>& data_blocks,
    DecodedShortMessage& message,
    std::string& error);

bool build_828s_utf16le_short_message(
    std::uint32_t source_id,
    std::uint32_t destination_id,
    bool group,
    const std::string& text_utf8,
    ShortMessagePdu& message,
    std::string& error);

} // namespace dmr_rpt
