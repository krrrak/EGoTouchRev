#pragma once

#include "ServiceConfigCore.h"
#include "Ipc/IpcProtocol.h"
#include "config/ConfigPath.h"
#include "config/ConfigStore.h"
#include "config/ConfigTlv.h"
#include "config/ConfigValue.h"
#include "config/ConfigSchemaSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Service {

class ConfigRuntime {
public:
    using StartupValidator = std::function<bool(const Config::ConfigStore&)>;

    struct TlvApplyResult {
        bool completed = false;
        Ipc::IpcStatusCode status = Ipc::IpcStatusCode::Ok;
        size_t entryCount = 0;
        size_t changedCount = 0;
        ServiceConfigState desiredServiceConfig{};
        Config::ConfigStore pipelineConfig{};
    };

    bool Initialize(const std::string& configPath, const StartupValidator& validateStartupConfig);
    Config::ConfigStore SnapshotStore() const;
    ServiceConfigState ServiceState() const;
    void WriteServiceState(const ServiceConfigState& config);
    bool PersistServicePolicyConfig(const ServiceConfigState& config);
    TlvApplyResult ApplyTlvChunk(const Ipc::ConfigTlvChunkRequestWire& chunk);

private:
    bool ValidateStartupConfig(const Config::ConfigStore& store, const StartupValidator& validateStartupConfig) const;
    ServiceConfigState ReadServiceConfigStateFromStoreLocked() const;
    void WriteServiceConfigStateToStoreLocked(const ServiceConfigState& config);
    bool ApplyCompletedTlvPayloadLocked(const std::vector<uint8_t>& payload, TlvApplyResult& result);

    // Lock boundary: ConfigRuntime owns config store/schema/paths/TLV session state.
    // Callers must not invoke DeviceRuntime or IPC callbacks while m_mutex is held.
    mutable std::mutex m_mutex;
    Config::ConfigStore m_defaults;
    Config::ConfigStore m_store;
    Config::ConfigSchemaSnapshot m_schema;
    std::optional<Config::ConfigPaths> m_paths;
    bool m_penButtonRouteExplicit = false;
    uint16_t m_pendingTlvSessionId = 0;
    uint16_t m_pendingTlvTotalLen = 0;
    uint16_t m_pendingTlvReceived = 0;
    std::vector<uint8_t> m_pendingTlvPayload;
};

} // namespace Service
