// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "dmr_rpt/event.h"

namespace dmr_rpt {

struct VectorFileEntry {
    std::string key;
    std::filesystem::path path;
    std::string expected_sha256;
    std::string actual_sha256;
    bool verified = false;
};

struct DmrVectorManifest {
    int schema_version = 0;
    std::string vector_id;
    std::string source_device;
    std::string profile;
    std::string air_case;
    int slot = 0;
    int color_code = -1;
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
    std::vector<VectorFileEntry> files;
};

struct ManifestVerification {
    bool ok = false;
    std::vector<std::string> errors;
    DmrVectorManifest manifest;
};

DmrVectorManifest load_vector_manifest(const std::filesystem::path& manifest_path);
ManifestVerification verify_vector_manifest(const std::filesystem::path& manifest_path,
                                            bool include_large_iq);
bool t1_t2_data_vectors_available(const std::filesystem::path& vector_root);

} // namespace dmr_rpt
