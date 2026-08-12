// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/short_message.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace dmr_rpt {
namespace {

constexpr std::uint8_t kDpfDefinedShortData = 0x0DU;
constexpr std::uint8_t kDpf828sShortData = 0x02U;
constexpr std::uint8_t kShortDataSap = 0x0AU;
constexpr std::uint8_t k828sShortDataSap = 0x03U;
constexpr std::uint8_t kUtf16BeDataFormat = 0x14U;
constexpr std::size_t k828sUserPrefixBytes = 6U;
constexpr std::size_t kRateHalfDataBytes = 12U;
constexpr std::size_t kRateHalfLastUserBytes = 8U;
constexpr std::size_t kRateHalfMessageCrcBytes = 4U;

std::uint32_t read_id(const std::array<std::uint8_t, 12>& payload,
                      std::size_t offset)
{
    return (static_cast<std::uint32_t>(payload[offset]) << 16U) |
           (static_cast<std::uint32_t>(payload[offset + 1U]) << 8U) |
           static_cast<std::uint32_t>(payload[offset + 2U]);
}

void write_id(std::array<std::uint8_t, 12>& payload,
              std::size_t offset,
              std::uint32_t id)
{
    payload[offset] = static_cast<std::uint8_t>(id >> 16U);
    payload[offset + 1U] = static_cast<std::uint8_t>(id >> 8U);
    payload[offset + 2U] = static_cast<std::uint8_t>(id);
}

void bytes_to_bits(const std::array<std::uint8_t, 12>& bytes,
                   std::array<std::uint8_t, 96>& bits)
{
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        for (unsigned bit = 0; bit < 8U; ++bit) {
            bits[index * 8U + bit] = static_cast<std::uint8_t>(
                (bytes[index] >> (7U - bit)) & 1U);
        }
    }
}

std::uint16_t crc16_ccitt(const std::array<std::uint8_t, 96>& bits)
{
    std::uint32_t crc = 0;
    constexpr std::uint32_t polynomial = 0x1021U;
    for (std::uint8_t bit : bits) {
        crc = (crc << 1U) | (bit & 1U);
        if ((crc & 0x10000U) != 0U) {
            crc = (crc & 0xFFFFU) ^ polynomial;
        }
    }
    return static_cast<std::uint16_t>((crc ^ 0xFFFFU) & 0xFFFFU);
}

void append_crc16_masked(std::array<std::uint8_t, 12>& header,
                         std::uint8_t mask)
{
    std::array<std::uint8_t, 96> bits{};
    for (std::uint32_t candidate = 0; candidate <= 0xFFFFU; ++candidate) {
        header[10] = static_cast<std::uint8_t>(candidate >> 8U);
        header[11] = static_cast<std::uint8_t>(candidate);
        bytes_to_bits(header, bits);
        if (crc16_ccitt(bits) == 0U) {
            header[10] ^= mask;
            header[11] ^= mask;
            return;
        }
    }
}

void append_header_crc(std::array<std::uint8_t, 12>& header)
{
    append_crc16_masked(header, 0xCCU);
}

bool has_crc16_masked(const std::array<std::uint8_t, 12>& payload,
                      std::uint8_t mask)
{
    std::array<std::uint8_t, 12> unmasked = payload;
    unmasked[10] ^= mask;
    unmasked[11] ^= mask;
    std::array<std::uint8_t, 96> bits{};
    bytes_to_bits(unmasked, bits);
    return crc16_ccitt(bits) == 0U;
}

std::uint32_t crc32_linear(const std::vector<std::uint8_t>& bytes)
{
    std::uint32_t crc = 0;
    constexpr std::uint32_t polynomial = 0x04C11DB7U;
    for (std::uint8_t byte : bytes) {
        crc ^= static_cast<std::uint32_t>(byte) << 24U;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc & 0x80000000U) != 0U
                ? (crc << 1U) ^ polynomial
                : (crc << 1U);
        }
    }
    return crc;
}

std::vector<std::uint8_t> dmr_crc_input(
    const std::vector<std::uint8_t>& user_data_and_pad)
{
    std::vector<std::uint8_t> linear;
    linear.reserve(user_data_and_pad.size());
    for (std::size_t index = 0; index < user_data_and_pad.size(); index += 2U) {
        linear.push_back(user_data_and_pad[index + 1U]);
        linear.push_back(user_data_and_pad[index]);
    }
    return linear;
}

std::uint32_t read_crc32_le(const std::array<std::uint8_t, 12>& block)
{
    return static_cast<std::uint32_t>(block[8]) |
           (static_cast<std::uint32_t>(block[9]) << 8U) |
           (static_cast<std::uint32_t>(block[10]) << 16U) |
           (static_cast<std::uint32_t>(block[11]) << 24U);
}

void write_crc32_le(std::array<std::uint8_t, 12>& block, std::uint32_t crc)
{
    block[8] = static_cast<std::uint8_t>(crc);
    block[9] = static_cast<std::uint8_t>(crc >> 8U);
    block[10] = static_cast<std::uint8_t>(crc >> 16U);
    block[11] = static_cast<std::uint8_t>(crc >> 24U);
}

bool append_utf8_codepoint(std::string& text, std::uint32_t codepoint)
{
    if (codepoint <= 0x7FU) {
        text.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        text.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        text.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        text.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        text.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0x10FFFFU) {
        text.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        text.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        return false;
    }
    return true;
}

bool decode_utf16be(const std::vector<std::uint8_t>& bytes,
                    std::string& text,
                    std::string& error)
{
    if ((bytes.size() % 2U) != 0U) {
        error = "UTF-16BE payload has an odd octet count";
        return false;
    }
    text.clear();
    for (std::size_t index = 0; index < bytes.size(); index += 2U) {
        const std::uint16_t unit =
            (static_cast<std::uint16_t>(bytes[index]) << 8U) |
            static_cast<std::uint16_t>(bytes[index + 1U]);
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (index + 3U >= bytes.size()) {
                error = "UTF-16BE payload ends with a high surrogate";
                return false;
            }
            const std::uint16_t low =
                (static_cast<std::uint16_t>(bytes[index + 2U]) << 8U) |
                static_cast<std::uint16_t>(bytes[index + 3U]);
            if (low < 0xDC00U || low > 0xDFFFU) {
                error = "UTF-16BE payload has an invalid surrogate pair";
                return false;
            }
            const std::uint32_t codepoint = 0x10000U +
                ((static_cast<std::uint32_t>(unit - 0xD800U) << 10U) |
                 static_cast<std::uint32_t>(low - 0xDC00U));
            if (!append_utf8_codepoint(text, codepoint)) {
                error = "UTF-16BE payload has an invalid codepoint";
                return false;
            }
            index += 2U;
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            error = "UTF-16BE payload has an unpaired low surrogate";
            return false;
        } else if (!append_utf8_codepoint(text, unit)) {
            error = "UTF-16BE payload has an invalid codepoint";
            return false;
        }
    }
    return true;
}

bool decode_utf16le(const std::vector<std::uint8_t>& bytes,
                    std::string& text,
                    std::string& error)
{
    if ((bytes.size() % 2U) != 0U) {
        error = "UTF-16LE payload has an odd octet count";
        return false;
    }
    std::vector<std::uint8_t> big_endian = bytes;
    for (std::size_t index = 0; index < big_endian.size(); index += 2U) {
        std::swap(big_endian[index], big_endian[index + 1U]);
    }
    return decode_utf16be(big_endian, text, error);
}

bool utf8_to_utf16be(const std::string& text,
                     std::vector<std::uint8_t>& bytes,
                     std::string& error)
{
    bytes.clear();
    for (std::size_t index = 0; index < text.size();) {
        const std::uint8_t first = static_cast<std::uint8_t>(text[index]);
        std::uint32_t codepoint = 0;
        std::size_t count = 0;
        if (first <= 0x7FU) {
            codepoint = first;
            count = 1;
        } else if ((first & 0xE0U) == 0xC0U) {
            codepoint = first & 0x1FU;
            count = 2;
        } else if ((first & 0xF0U) == 0xE0U) {
            codepoint = first & 0x0FU;
            count = 3;
        } else if ((first & 0xF8U) == 0xF0U) {
            codepoint = first & 0x07U;
            count = 4;
        } else {
            error = "text is not valid UTF-8";
            return false;
        }
        if (index + count > text.size()) {
            error = "text is not valid UTF-8";
            return false;
        }
        for (std::size_t continuation = 1; continuation < count; ++continuation) {
            const std::uint8_t byte =
                static_cast<std::uint8_t>(text[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                error = "text is not valid UTF-8";
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        const std::uint32_t minimum = count == 1U ? 0U :
            (count == 2U ? 0x80U : (count == 3U ? 0x800U : 0x10000U));
        if (codepoint < minimum || codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            error = "text is not valid UTF-8";
            return false;
        }
        if (codepoint <= 0xFFFFU) {
            bytes.push_back(static_cast<std::uint8_t>(codepoint >> 8U));
            bytes.push_back(static_cast<std::uint8_t>(codepoint));
        } else {
            const std::uint32_t surrogate = codepoint - 0x10000U;
            const std::uint16_t high = static_cast<std::uint16_t>(
                0xD800U | (surrogate >> 10U));
            const std::uint16_t low = static_cast<std::uint16_t>(
                0xDC00U | (surrogate & 0x3FFU));
            bytes.push_back(static_cast<std::uint8_t>(high >> 8U));
            bytes.push_back(static_cast<std::uint8_t>(high));
            bytes.push_back(static_cast<std::uint8_t>(low >> 8U));
            bytes.push_back(static_cast<std::uint8_t>(low));
        }
        index += count;
    }
    return true;
}

bool utf8_to_utf16le(const std::string& text,
                     std::vector<std::uint8_t>& bytes,
                     std::string& error)
{
    if (!utf8_to_utf16be(text, bytes, error)) {
        return false;
    }
    for (std::size_t index = 0; index < bytes.size(); index += 2U) {
        std::swap(bytes[index], bytes[index + 1U]);
    }
    return true;
}

} // namespace

bool parse_defined_short_message_header(
    const std::array<std::uint8_t, 12>& payload,
    ShortMessageHeader& header,
    std::string& error)
{
    if ((payload[0] & 0x0FU) != kDpfDefinedShortData) {
        error = "data header is not defined short data";
        return false;
    }
    header.group = (payload[0] & 0x80U) != 0U;
    header.response_requested = (payload[0] & 0x40U) != 0U;
    header.appended_blocks = static_cast<std::uint8_t>(
        ((payload[0] & 0x30U) << 2U) | (payload[1] & 0x0FU));
    header.sap = static_cast<std::uint8_t>(payload[1] >> 4U);
    header.destination_id = read_id(payload, 2U);
    header.source_id = read_id(payload, 5U);
    header.defined_data_format = static_cast<std::uint8_t>(payload[8] >> 2U);
    header.sarq = (payload[8] & 0x02U) != 0U;
    header.full_message = (payload[8] & 0x01U) != 0U;
    header.pad_octets = payload[9];
    if (header.sap != kShortDataSap) {
        error = "defined short data header has an unexpected SAP";
        return false;
    }
    if (header.appended_blocks == 0U) {
        error = "defined short data header has no data blocks";
        return false;
    }
    if (!header.full_message) {
        error = "fragmented defined short data is not supported";
        return false;
    }
    return true;
}

bool decode_unconfirmed_rate_half_short_message(
    const ShortMessageHeader& header,
    const std::vector<std::array<std::uint8_t, 12>>& data_blocks,
    DecodedShortMessage& message,
    std::string& error)
{
    if (data_blocks.size() != header.appended_blocks) {
        error = "defined short data block count does not match header";
        return false;
    }
    if (header.pad_octets > kRateHalfLastUserBytes) {
        error = "defined short data pad octet count is invalid";
        return false;
    }

    std::vector<std::uint8_t> data_with_pad;
    data_with_pad.reserve((data_blocks.size() - 1U) * kRateHalfDataBytes +
                          kRateHalfLastUserBytes);
    for (std::size_t index = 0; index + 1U < data_blocks.size(); ++index) {
        data_with_pad.insert(data_with_pad.end(), data_blocks[index].begin(),
                             data_blocks[index].end());
    }
    data_with_pad.insert(data_with_pad.end(), data_blocks.back().begin(),
                         data_blocks.back().begin() + kRateHalfLastUserBytes);

    const std::uint32_t expected_crc = crc32_linear(dmr_crc_input(data_with_pad));
    if (read_crc32_le(data_blocks.back()) != expected_crc) {
        error = "defined short data message CRC failed";
        return false;
    }
    if (header.pad_octets > data_with_pad.size()) {
        error = "defined short data pad octet count exceeds message";
        return false;
    }
    data_with_pad.resize(data_with_pad.size() - header.pad_octets);

    message = {};
    message.header = header;
    message.user_data = std::move(data_with_pad);
    if (header.defined_data_format == kUtf16BeDataFormat &&
        !decode_utf16be(message.user_data, message.text_utf8, error)) {
        return false;
    }
    return true;
}

bool build_unconfirmed_utf16be_short_message(
    std::uint32_t source_id,
    std::uint32_t destination_id,
    bool group,
    const std::string& text_utf8,
    ShortMessagePdu& message,
    std::string& error)
{
    if (source_id == 0U || source_id > 0xFFFFFFU || destination_id == 0U ||
        destination_id > 0xFFFFFFU) {
        error = "DMR source and destination IDs must be 1..0xFFFFFF";
        return false;
    }
    std::vector<std::uint8_t> user_data;
    if (text_utf8.empty() || !utf8_to_utf16be(text_utf8, user_data, error)) {
        if (text_utf8.empty()) {
            error = "short message text is empty";
        }
        return false;
    }

    const std::size_t required_data_bytes =
        user_data.size() + kRateHalfMessageCrcBytes;
    const std::size_t data_blocks = 1U +
        (required_data_bytes > kRateHalfLastUserBytes
             ? (required_data_bytes - kRateHalfLastUserBytes +
                kRateHalfDataBytes - 1U) / kRateHalfDataBytes
             : 0U);
    if (data_blocks == 0U || data_blocks > 63U) {
        error = "short message is too long for rate 1/2 defined data";
        return false;
    }
    const std::size_t capacity = (data_blocks - 1U) * kRateHalfDataBytes +
        kRateHalfLastUserBytes;
    const std::size_t pad_octets = capacity - user_data.size();
    if (pad_octets > kRateHalfLastUserBytes || (user_data.size() % 2U) != 0U) {
        error = "short message cannot be packed as UTF-16BE rate 1/2 data";
        return false;
    }
    user_data.resize(capacity, 0U);

    message = {};
    message.header[0] = static_cast<std::uint8_t>(
        (group ? 0x80U : 0U) |
        ((static_cast<std::uint8_t>(data_blocks) >> 4U) << 4U) |
        kDpfDefinedShortData);
    message.header[1] = static_cast<std::uint8_t>(
        (kShortDataSap << 4U) | (static_cast<std::uint8_t>(data_blocks) & 0x0FU));
    write_id(message.header, 2U, destination_id);
    write_id(message.header, 5U, source_id);
    message.header[8] = static_cast<std::uint8_t>((kUtf16BeDataFormat << 2U) | 0x01U);
    message.header[9] = static_cast<std::uint8_t>(pad_octets);
    append_header_crc(message.header);

    message.data_blocks.resize(data_blocks);
    std::size_t offset = 0;
    for (std::size_t index = 0; index < data_blocks; ++index) {
        const std::size_t count = index + 1U == data_blocks
            ? kRateHalfLastUserBytes
            : kRateHalfDataBytes;
        std::copy_n(user_data.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    message.data_blocks[index].begin());
        offset += count;
    }
    write_crc32_le(message.data_blocks.back(),
                   crc32_linear(dmr_crc_input(user_data)));
    return true;
}

std::array<std::uint8_t, 12> build_828s_data_preamble(
    std::uint32_t source_id,
    std::uint32_t destination_id,
    bool group)
{
    std::array<std::uint8_t, 12> preamble{};
    preamble[0] = static_cast<std::uint8_t>(group ? 0xBDU : 0x3DU);
    preamble[2] = group ? 0xC0U : 0x80U;
    write_id(preamble, 4U, destination_id);
    write_id(preamble, 7U, source_id);
    append_crc16_masked(preamble, 0xA5U);
    return preamble;
}

bool parse_828s_data_preamble(
    const std::array<std::uint8_t, 12>& payload,
    ShortMessagePreamble& preamble,
    std::string& error)
{
    const bool group = payload[0] == 0xBDU && payload[2] == 0xC0U;
    const bool private_call = payload[0] == 0x3DU && payload[2] == 0x80U;
    if ((!group && !private_call) || !has_crc16_masked(payload, 0xA5U)) {
        error = "data burst is not an 828S data preamble";
        return false;
    }
    preamble = {};
    preamble.group = group;
    preamble.destination_id = read_id(payload, 4U);
    preamble.source_id = read_id(payload, 7U);
    if (preamble.destination_id == 0U || preamble.source_id == 0U) {
        error = "828S data preamble has invalid addressing";
        return false;
    }
    return true;
}

bool build_828s_short_message_header(
    const ShortMessagePreamble& preamble,
    std::uint8_t appended_blocks,
    std::array<std::uint8_t, 12>& header,
    std::string& error)
{
    if (preamble.source_id == 0U || preamble.source_id > 0xFFFFFFU ||
        preamble.destination_id == 0U || preamble.destination_id > 0xFFFFFFU ||
        appended_blocks == 0U || appended_blocks > 15U) {
        error = "828S short data header has invalid preamble or block count";
        return false;
    }
    header = {};
    header[0] = static_cast<std::uint8_t>(
        (preamble.group ? 0x80U : 0U) | kDpf828sShortData);
    // UHF 828S accepts the verified 0x3B data-header variant.  The
    // 0x31/0x33 variants are retained for RX compatibility because they are
    // emitted by the VHF 828S firmware.
    header[1] = 0x3BU;
    write_id(header, 2U, preamble.destination_id);
    write_id(header, 5U, preamble.source_id);
    header[8] = static_cast<std::uint8_t>(0x80U | appended_blocks);
    append_header_crc(header);
    return true;
}

bool parse_828s_short_message_header(
    const std::array<std::uint8_t, 12>& payload,
    ShortMessageHeader& header,
    std::string& error)
{
    const std::uint8_t block_count = payload[8] & 0x0FU;
    if ((payload[0] & 0x0FU) != kDpf828sShortData ||
        (payload[1] != 0x3BU && payload[1] != 0x31U &&
         payload[1] != 0x33U) ||
        (payload[8] & 0xF0U) != 0x80U ||
        block_count == 0U || payload[9] != 0U) {
        error = "data header is not 828S short data";
        return false;
    }
    header = {};
    header.group = (payload[0] & 0x80U) != 0U;
    header.response_requested = (payload[0] & 0x40U) != 0U;
    header.appended_blocks = block_count;
    header.sap = static_cast<std::uint8_t>(payload[1] >> 4U);
    header.destination_id = read_id(payload, 2U);
    header.source_id = read_id(payload, 5U);
    header.defined_data_format = kUtf16BeDataFormat;
    header.full_message = true;
    if (header.destination_id == 0U || header.source_id == 0U ||
        header.sap != k828sShortDataSap) {
        error = "828S short data header has invalid addressing";
        return false;
    }
    return true;
}

bool decode_828s_short_message(
    const ShortMessageHeader& header,
    const std::vector<std::array<std::uint8_t, 12>>& data_blocks,
    DecodedShortMessage& message,
    std::string& error)
{
    if (header.appended_blocks == 0U ||
        data_blocks.size() != header.appended_blocks) {
        error = "828S short data block count does not match header";
        return false;
    }

    const std::size_t data_size =
        (data_blocks.size() - 1U) * kRateHalfDataBytes +
        kRateHalfLastUserBytes;
    std::vector<std::uint8_t> data(data_size, 0U);
    for (std::size_t index = 0; index < data_blocks.size(); ++index) {
        const std::size_t count = index + 1U == data_blocks.size()
            ? kRateHalfLastUserBytes
            : kRateHalfDataBytes;
        std::copy_n(data_blocks[index].begin(), count,
                    data.begin() + static_cast<std::ptrdiff_t>(
                        index * kRateHalfDataBytes));
    }
    const std::uint32_t expected_crc = crc32_linear(dmr_crc_input(data));
    if (read_crc32_le(data_blocks.back()) != expected_crc) {
        error = "828S short message CRC failed";
        return false;
    }
    if (data[1] != 0U || data[3] != 1U || data[4] != 1U || data[5] != 0U) {
        error = "828S short message data marker is invalid";
        return false;
    }
    if (data[2] != 2U) {
        error = "828S short message text length is invalid";
        return false;
    }

    // The first octet changes between otherwise identical 828S messages.
    // Text length is therefore delimited by UTF-16LE zero-unit padding.
    std::size_t text_end = data.size();
    while (text_end >= k828sUserPrefixBytes + 2U &&
           data[text_end - 2U] == 0U && data[text_end - 1U] == 0U) {
        text_end -= 2U;
    }
    if (text_end == k828sUserPrefixBytes) {
        error = "828S short message text is empty";
        return false;
    }

    std::vector<std::uint8_t> user_data(
        data.begin() + static_cast<std::ptrdiff_t>(k828sUserPrefixBytes),
        data.begin() + static_cast<std::ptrdiff_t>(text_end));
    message = {};
    message.header = header;
    message.user_data = user_data;
    return decode_utf16le(user_data, message.text_utf8, error);
}

bool recover_828s_utf16le_short_message(
    const ShortMessageHeader& header,
    const std::vector<std::array<std::uint8_t, 12>>& data_blocks,
    DecodedShortMessage& message,
    std::string& error)
{
    if (data_blocks.empty() || data_blocks.size() > 15U) {
        error = "828S recovery has an invalid data block count";
        return false;
    }

    const std::size_t data_size =
        (data_blocks.size() - 1U) * kRateHalfDataBytes +
        kRateHalfLastUserBytes;
    std::vector<std::uint8_t> data(data_size, 0U);
    for (std::size_t index = 0; index < data_blocks.size(); ++index) {
        const std::size_t count = index + 1U == data_blocks.size()
            ? kRateHalfLastUserBytes
            : kRateHalfDataBytes;
        std::copy_n(data_blocks[index].begin(), count,
                    data.begin() + static_cast<std::ptrdiff_t>(
                        index * kRateHalfDataBytes));
    }

    if (data.size() <= k828sUserPrefixBytes || data[3] != 1U ||
        data[4] != 1U || data[5] != 0U) {
        error = "828S recovery data marker is invalid";
        return false;
    }

    std::size_t text_end = data.size();
    while (text_end >= k828sUserPrefixBytes + 2U &&
           data[text_end - 2U] == 0U && data[text_end - 1U] == 0U) {
        text_end -= 2U;
    }
    if (text_end == k828sUserPrefixBytes) {
        error = "828S recovery text is empty";
        return false;
    }

    std::vector<std::uint8_t> user_data(
        data.begin() + static_cast<std::ptrdiff_t>(k828sUserPrefixBytes),
        data.begin() + static_cast<std::ptrdiff_t>(text_end));
    message = {};
    message.header = header;
    message.user_data = user_data;
    return decode_utf16le(user_data, message.text_utf8, error);
}

bool build_828s_utf16le_short_message(
    std::uint32_t source_id,
    std::uint32_t destination_id,
    bool group,
    const std::string& text_utf8,
    ShortMessagePdu& message,
    std::string& error)
{
    if (source_id == 0U || source_id > 0xFFFFFFU || destination_id == 0U ||
        destination_id > 0xFFFFFFU) {
        error = "DMR source and destination IDs must be 1..0xFFFFFF";
        return false;
    }
    std::vector<std::uint8_t> user_data;
    if (text_utf8.empty() || !utf8_to_utf16le(text_utf8, user_data, error)) {
        if (text_utf8.empty()) {
            error = "short message text is empty";
        }
        return false;
    }
    if ((user_data.size() % 2U) != 0U || user_data.size() > 0xFFF0U) {
        error = "828S short message text cannot be encoded";
        return false;
    }

    // A rate-1/2 data block reserves its final four physical octets for the
    // message CRC.  The UTF-16LE terminator belongs in the preceding eight
    // data octets, not in that CRC reservation.  Counting the CRC as user
    // capacity adds a spurious block at the six-character and 36-byte edges.
    const std::size_t payload_data_size =
        k828sUserPrefixBytes + user_data.size() + 2U;
    const std::size_t data_blocks = 1U +
        (payload_data_size > kRateHalfLastUserBytes
             ? (payload_data_size - kRateHalfLastUserBytes +
                kRateHalfDataBytes - 1U) / kRateHalfDataBytes
             : 0U);
    if (data_blocks > 15U) {
        error = "828S short message is too long for one air packet";
        return false;
    }
    const std::size_t data_size =
        (data_blocks - 1U) * kRateHalfDataBytes + kRateHalfLastUserBytes;
    std::vector<std::uint8_t> data(data_size, 0U);
    data[0] = static_cast<std::uint8_t>(k828sUserPrefixBytes + user_data.size() +
                                        kRateHalfMessageCrcBytes);
    data[2] = 2U;
    data[3] = 1U;
    data[4] = 1U;
    std::copy(user_data.begin(), user_data.end(),
              data.begin() + k828sUserPrefixBytes);

    message = {};
    if (!build_828s_short_message_header(
            ShortMessagePreamble{group, destination_id, source_id},
            static_cast<std::uint8_t>(data_blocks), message.header, error)) {
        return false;
    }

    message.data_blocks.resize(data_blocks);
    for (std::size_t index = 0; index < data_blocks; ++index) {
        const std::size_t count = index + 1U == data_blocks
            ? kRateHalfLastUserBytes
            : kRateHalfDataBytes;
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(
                        index * kRateHalfDataBytes),
                    count, message.data_blocks[index].begin());
    }
    write_crc32_le(message.data_blocks.back(), crc32_linear(dmr_crc_input(data)));
    return true;
}

} // namespace dmr_rpt
