// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/vector_manifest.h"

#include "dmr_rpt/config.h"
#include "dmr_rpt/sha256.h"

#include <algorithm>
#include <cctype>

#include <yaml-cpp/yaml.h>

namespace dmr_rpt {
namespace {

std::string lower_hex(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

VectorFileEntry parse_file_entry(const YAML::Node& files,
                                 const char* key,
                                 const std::filesystem::path& base)
{
    const YAML::Node node = files[key];
    if (!node || !node.IsMap()) {
        throw ConfigError(std::string("manifest files.") + key + " is required");
    }
    VectorFileEntry entry;
    entry.key = key;
    entry.path = base / node["path"].as<std::string>();
    entry.expected_sha256 = lower_hex(node["sha256"].as<std::string>());
    return entry;
}

} // namespace

DmrVectorManifest load_vector_manifest(const std::filesystem::path& manifest_path)
{
    const std::filesystem::path base = manifest_path.parent_path();
    YAML::Node root = YAML::LoadFile(manifest_path.string());
    if (!root.IsMap()) {
        throw ConfigError("manifest root must be a JSON/YAML object");
    }

    DmrVectorManifest manifest;
    manifest.schema_version = root["schema_version"].as<int>();
    manifest.vector_id = root["vector_id"].as<std::string>();
    manifest.source_device = root["source_device"].as<std::string>();
    manifest.profile = root["profile"].as<std::string>();
    manifest.air_case = root["air_case"].as<std::string>();
    const YAML::Node dmr = root["dmr"];
    if (!dmr || !dmr.IsMap()) {
        throw ConfigError("manifest.dmr is required");
    }
    manifest.slot = dmr["slot"].as<int>();
    manifest.color_code = dmr["color_code"].as<int>();
    manifest.source_id = dmr["source_id"].as<std::uint32_t>();
    manifest.destination_id = dmr["target_id"].as<std::uint32_t>();

    const YAML::Node files = root["files"];
    if (!files || !files.IsMap()) {
        throw ConfigError("manifest.files is required");
    }
    manifest.files.push_back(parse_file_entry(files, "input_dibits", base));
    manifest.files.push_back(parse_file_entry(files, "expected", base));
    manifest.files.push_back(parse_file_entry(files, "rx_iq", base));
    return manifest;
}

ManifestVerification verify_vector_manifest(const std::filesystem::path& manifest_path,
                                            bool include_large_iq)
{
    ManifestVerification verification;
    try {
        verification.manifest = load_vector_manifest(manifest_path);
        if (verification.manifest.schema_version != 1) {
            verification.errors.push_back("unsupported schema_version");
        }
        if (verification.manifest.source_device != "828S") {
            verification.errors.push_back("source_device must be 828S");
        }
        if (verification.manifest.vector_id.empty()) {
            verification.errors.push_back("vector_id is empty");
        }
        for (VectorFileEntry& entry : verification.manifest.files) {
            if (entry.key == "rx_iq" && !include_large_iq) {
                continue;
            }
            entry.actual_sha256 = lower_hex(sha256_file_hex(entry.path));
            entry.verified = entry.actual_sha256 == entry.expected_sha256;
            if (!entry.verified) {
                verification.errors.push_back(entry.key + " SHA-256 mismatch");
            }
        }
    } catch (const std::exception& error) {
        verification.errors.push_back(error.what());
    }
    verification.ok = verification.errors.empty();
    return verification;
}

bool t1_t2_data_vectors_available(const std::filesystem::path& vector_root)
{
    bool has_t1 = false;
    bool has_t2 = false;
    bool has_data = false;
    if (!std::filesystem::exists(vector_root)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(vector_root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto manifest_path = entry.path() / "manifest.json";
        if (!std::filesystem::exists(manifest_path)) {
            continue;
        }
        try {
            const DmrVectorManifest manifest = load_vector_manifest(manifest_path);
            if (manifest.profile == "t1") {
                has_t1 = true;
            } else if (manifest.profile == "t2") {
                has_t2 = true;
            }
            if (manifest.air_case.find("data") != std::string::npos) {
                has_data = true;
            }
        } catch (const std::exception&) {
        }
    }
    return has_t1 && has_t2 && has_data;
}

} // namespace dmr_rpt
