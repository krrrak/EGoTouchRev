#include "config/YamlParser.h"

#include "Logger.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace Config {

YAML::Node YamlParser::load(const std::string& filePath) {
    if (!std::filesystem::exists(filePath)) {
        LOG_ERROR("Config", __func__, "YAML", "YAML file not found: {}", filePath);
        throw std::runtime_error("YamlParser: file not found: " + filePath);
    }

    try {
        return YAML::LoadFile(filePath);
    } catch (const YAML::ParserException& ex) {
        LOG_ERROR("Config", __func__, "YAML", "YAML format error in {} at line {}, column {}: {}",
                  filePath, ex.mark.line + 1, ex.mark.column + 1, ex.msg);
        throw std::runtime_error("YamlParser: format error in " + filePath + ": " + ex.msg);
    } catch (const YAML::BadFile& ex) {
        LOG_ERROR("Config", __func__, "YAML", "Failed to open YAML file {}: {}", filePath, ex.what());
        throw std::runtime_error("YamlParser: failed to open file: " + filePath);
    } catch (const YAML::Exception& ex) {
        LOG_ERROR("Config", __func__, "YAML", "Failed to load YAML file {}: {}", filePath, ex.what());
        throw std::runtime_error("YamlParser: failed to load " + filePath + ": " + ex.what());
    }
}

void YamlParser::save(const std::string& filePath, const YAML::Node& node) {
    try {
        YAML::Emitter emitter;
        emitter.SetIndent(2);
        emitter.SetMapFormat(YAML::Block);
        emitter.SetSeqFormat(YAML::Block);
        emitter << node;

        if (!emitter.good()) {
            LOG_ERROR("Config", __func__, "YAML", "Failed to emit YAML for {}: {}", filePath, emitter.GetLastError());
            throw std::runtime_error("YamlParser: failed to emit YAML: " + emitter.GetLastError());
        }

        std::ofstream output(filePath, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            LOG_ERROR("Config", __func__, "YAML", "Failed to open YAML file for writing: {}", filePath);
            throw std::runtime_error("YamlParser: failed to open file for writing: " + filePath);
        }

        output << emitter.c_str() << '\n';
        if (!output.good()) {
            LOG_ERROR("Config", __func__, "YAML", "Failed to write YAML file: {}", filePath);
            throw std::runtime_error("YamlParser: failed to write file: " + filePath);
        }
    } catch (const YAML::Exception& ex) {
        LOG_ERROR("Config", __func__, "YAML", "Failed to save YAML file {}: {}", filePath, ex.what());
        throw std::runtime_error("YamlParser: failed to save " + filePath + ": " + ex.what());
    }
}

YAML::Node YamlParser::merge(const YAML::Node& base, const YAML::Node& overlay) {
    if (!overlay || overlay.IsNull()) {
        return YAML::Clone(base);
    }
    if (!base || base.IsNull()) {
        return YAML::Clone(overlay);
    }

    if (!base.IsMap() || !overlay.IsMap()) {
        return YAML::Clone(overlay);
    }

    YAML::Node merged = YAML::Clone(base);
    for (const auto& item : overlay) {
        const auto key = item.first.as<std::string>();
        merged[key] = merge(merged[key], item.second);
    }
    return merged;
}

} // namespace Config
