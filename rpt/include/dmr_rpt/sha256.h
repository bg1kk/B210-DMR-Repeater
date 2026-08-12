// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace dmr_rpt {

std::string sha256_hex(std::string_view data);
std::string sha256_file_hex(const std::filesystem::path& path);

} // namespace dmr_rpt
