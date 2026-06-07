#include "ConfigRuntime.h"

#include "config/ConfigKeyMap.h"

#include <cassert>
#include <cstring>
#include <vector>

int main() {
    Service::ConfigRuntime runtime;
    assert(runtime.Initialize("", [](const Config::ConfigStore&) { return true; }));

    const auto initial = runtime.ServiceState();
    assert(initial.autoMode);

    auto keyId = Config::tryKeyIdForPath("service.auto_mode");
    assert(keyId.has_value());

    Config::ConfigPatchTlv patch{};
    patch.entries.push_back(Config::ConfigTlvEntry{*keyId, Config::ConfigValueType::Bool, "false"});
    const auto payload = Config::serializePatch(patch);

    Ipc::ConfigTlvChunkRequestWire chunk{};
    chunk.wireVersion = Ipc::kIpcProtocolVersion;
    chunk.sessionId = 7;
    chunk.flags = Ipc::kConfigTlvChunkFirst | Ipc::kConfigTlvChunkLast;
    chunk.totalLen = static_cast<uint16_t>(payload.size());
    chunk.chunkLen = static_cast<uint16_t>(payload.size());
    std::memcpy(chunk.bytes, payload.data(), payload.size());

    const auto applied = runtime.ApplyTlvChunk(chunk);
    assert(applied.status == Ipc::IpcStatusCode::Ok);
    assert(applied.completed);
    assert(applied.changedCount == 1);
    assert(!applied.desiredServiceConfig.autoMode);

    chunk.sessionId = 8;
    chunk.bytes[0] ^= 0xFF;
    const auto rejected = runtime.ApplyTlvChunk(chunk);
    assert(rejected.status == Ipc::IpcStatusCode::InvalidRequest);
    assert(!runtime.ServiceState().autoMode);

    return 0;
}
