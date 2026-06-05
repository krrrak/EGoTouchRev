#include "config/ConfigKeyMap.h"

#include <utility>

namespace Config {
namespace {

std::unordered_map<ConfigKeyId, std::string>& mutableKeyIdToPath()
{
    static std::unordered_map<ConfigKeyId, std::string> map;
    return map;
}

std::unordered_map<std::string, ConfigKeyId>& mutablePathToKeyId()
{
    static std::unordered_map<std::string, ConfigKeyId> map;
    return map;
}

} // namespace

const std::unordered_map<ConfigKeyId, std::string>& keyIdToPath()
{
    return mutableKeyIdToPath();
}

const std::unordered_map<std::string, ConfigKeyId>& pathToKeyId()
{
    return mutablePathToKeyId();
}

void registerKeyMapping(ConfigKeyId id, std::string_view yamlPath)
{
    auto& idToPath = mutableKeyIdToPath();
    auto& pathToId = mutablePathToKeyId();

    if (const auto existing = idToPath.find(id); existing != idToPath.end()) {
        pathToId.erase(existing->second);
    }

    std::string path{yamlPath};
    idToPath[id] = path;
    pathToId[std::move(path)] = id;
}

} // namespace Config
