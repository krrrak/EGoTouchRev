#pragma once

#include "ConfigKeyId.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace Config {

// keyId → YAML path
const std::unordered_map<ConfigKeyId, std::string>& keyIdToPath();

// YAML path → keyId
const std::unordered_map<std::string, ConfigKeyId>& pathToKeyId();

// 注册映射（由 ConfigBinder 的 bind() 自动调用）
void registerKeyMapping(ConfigKeyId id, std::string_view yamlPath);

} // namespace Config
