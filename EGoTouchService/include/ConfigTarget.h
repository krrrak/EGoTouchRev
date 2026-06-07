#pragma once

#include "ServiceConfigCore.h"
#include "config/ConfigKeyId.h"
#include "config/ConfigStore.h"
#include "config/ConfigValue.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Service {

enum class ConfigApplyPhase {
    Live,
    Startup,
};

enum class ConfigApplyActionKind {
    ServicePolicy,
    PipelineRuntime,
};

struct ConfigChange {
    std::string path;
    Config::ConfigKeyId keyId = Config::ConfigKeyId::MaxKeyId;
    Config::ConfigValue previousValue;
    Config::ConfigValue newValue;
    bool hadPreviousValue = false;
};

struct ConfigChangeSet {
    std::vector<ConfigChange> changes;
    size_t entryCount = 0;

    bool empty() const noexcept { return changes.empty(); }
    size_t changedCount() const noexcept { return changes.size(); }

    bool containsPath(std::string_view path) const {
        for (const auto& change : changes) {
            if (change.path == path) return true;
        }
        return false;
    }

    bool containsPrefix(std::string_view prefix) const {
        for (const auto& change : changes) {
            if (change.path.size() >= prefix.size() &&
                std::string_view(change.path).substr(0, prefix.size()) == prefix) {
                return true;
            }
        }
        return false;
    }
};

struct ConfigApplyAction {
    ConfigApplyActionKind kind = ConfigApplyActionKind::PipelineRuntime;
    std::string targetName;
    Config::ConfigStore configStore;
    ServiceConfigState serviceConfig{};
};

struct ConfigTargetResult {
    std::string targetName;
    ConfigApplyPhase phase = ConfigApplyPhase::Live;
    bool ok = true;
    std::string message;
    std::vector<ConfigApplyAction> actions;
};

class IConfigTarget {
public:
    virtual ~IConfigTarget() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual bool isInterested(const ConfigChangeSet& changeSet) const = 0;
    virtual ConfigTargetResult validateConfig(const Config::ConfigStore& candidate,
                                              const ConfigChangeSet& changeSet) const = 0;
    virtual ConfigTargetResult applyConfig(const Config::ConfigStore& candidate,
                                           const ConfigChangeSet& changeSet,
                                           ConfigApplyPhase phase) const = 0;
};

} // namespace Service
