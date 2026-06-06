#pragma once

#include "ConfigKeyId.h"
#include "ConfigSchemaSnapshot.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Config {

class ConfigBinder;
class ConfigStore;

// keyId → YAML path
const std::unordered_map<ConfigKeyId, std::string>& keyIdToPath();

// YAML path → keyId
const std::unordered_map<std::string, ConfigKeyId>& pathToKeyId();

// 注册映射（由 ConfigBinder 的 bind() 自动调用）
void registerKeyMapping(ConfigKeyId id, std::string_view yamlPath);

// 为 binder 中尚未静态映射的 runtime key 分配确定性 keyId。
void registerRuntimeKeyMappings(const ConfigBinder& binder);

// 静态查找 keyId → path (需要先填充映射表)
std::optional<std::string_view> tryPathForKeyId(ConfigKeyId id);

// 静态查找 path → keyId (需要先填充映射表)
std::optional<ConfigKeyId> tryKeyIdForPath(std::string_view yamlPath);

// 构建全量 schema: defaults 提供所有 key + 默认值，binder 提供 live value + range + description
ConfigSchemaSnapshot BuildMergedSchema(const ConfigStore& defaults, const ConfigBinder& binder);

} // namespace Config
