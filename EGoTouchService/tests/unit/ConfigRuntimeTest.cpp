#include "ConfigRuntime.h"

#include "config/ConfigKeyMap.h"
#include "config/ConfigTlv.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool CatalogHasPath(const Config::ConfigV3CatalogPayload& catalog, std::string_view path) {
    return std::any_of(catalog.entries.begin(), catalog.entries.end(),
        [path](const Config::ConfigDescriptor& entry) { return entry.path == path; });
}

const Config::ConfigV3SnapshotEntry* FindSnapshotEntry(const Config::ConfigV3SnapshotPayload& snapshot, Config::ConfigKeyId keyId) {
    const auto it = std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
        [keyId](const Config::ConfigV3SnapshotEntry& entry) { return entry.keyId == keyId; });
    return it == snapshot.entries.end() ? nullptr : &*it;
}

bool HasAction(const Service::ConfigRuntime::TlvApplyResult& result, Service::ConfigApplyActionKind kind) {
    return std::any_of(result.applyActions.begin(), result.applyActions.end(),
        [kind](const Service::ConfigApplyAction& action) { return action.kind == kind; });
}

bool HasFailedTargetResult(const Service::ConfigRuntime::TlvApplyResult& result, std::string_view targetName) {
    return std::any_of(result.targetResults.begin(), result.targetResults.end(),
        [targetName](const Service::ConfigTargetResult& targetResult) {
            return targetResult.targetName == targetName && !targetResult.ok;
        });
}

Ipc::ConfigTlvChunkRequestWire MakeSingleChunk(uint16_t sessionId, const std::vector<uint8_t>& payload) {
    Ipc::ConfigTlvChunkRequestWire chunk{};
    chunk.wireVersion = Ipc::kIpcProtocolVersion;
    chunk.sessionId = sessionId;
    chunk.flags = Ipc::kConfigTlvChunkFirst | Ipc::kConfigTlvChunkLast;
    chunk.totalLen = static_cast<uint16_t>(payload.size());
    chunk.chunkLen = static_cast<uint16_t>(payload.size());
    std::memcpy(chunk.bytes, payload.data(), payload.size());
    return chunk;
}

std::vector<uint8_t> MakePatchPayload(std::initializer_list<Config::ConfigTlvEntry> entries) {
    Config::ConfigPatchTlv patch{};
    patch.entries.insert(patch.entries.end(), entries.begin(), entries.end());
    return Config::serializePatch(patch);
}

class RejectAutoModeTarget final : public Service::IConfigTarget {
public:
    std::string_view name() const noexcept override { return "RejectAutoModeTarget"; }

    bool isInterested(const Service::ConfigChangeSet& changeSet) const override {
        return changeSet.containsPath("service.auto_mode");
    }

    Service::ConfigTargetResult validateConfig(const Config::ConfigStore&,
                                               const Service::ConfigChangeSet&) const override {
        Service::ConfigTargetResult result{};
        result.targetName = std::string(name());
        result.phase = Service::ConfigApplyPhase::Live;
        result.ok = false;
        result.message = "test target rejection";
        return result;
    }

    Service::ConfigTargetResult applyConfig(const Config::ConfigStore&,
                                            const Service::ConfigChangeSet&,
                                            Service::ConfigApplyPhase phase) const override {
        Service::ConfigTargetResult result{};
        result.targetName = std::string(name());
        result.phase = phase;
        result.ok = true;
        return result;
    }
};

} // namespace

int main() {
    Service::ConfigRuntime runtime;
    assert(runtime.Initialize("", [](const Config::ConfigStore&) { return true; }));

    const auto initial = runtime.ServiceState();
    assert(initial.autoMode);

    const auto catalogBlob = runtime.BuildCatalogV3Blob();
    assert(!catalogBlob.bytes.empty());
    assert(catalogBlob.schemaVersion != 0);
    assert(catalogBlob.checksum != 0);
    const auto catalog = Config::deserializeConfigV3Catalog(catalogBlob.bytes.data(), catalogBlob.bytes.size());
    assert(catalog.schemaVersion == catalogBlob.schemaVersion);
    assert(CatalogHasPath(catalog, "service.mode"));
    assert(CatalogHasPath(catalog, "touch.signal_cond.baseline_no_finger_max_step"));
    assert(CatalogHasPath(catalog, "stylus.sp.iir_max_coef"));

    const auto snapshotBlob = runtime.BuildSnapshotV3Blob();
    assert(!snapshotBlob.bytes.empty());
    assert(snapshotBlob.schemaVersion == catalogBlob.schemaVersion);
    assert(snapshotBlob.snapshotVersion != 0);
    assert(snapshotBlob.checksum != 0);
    const auto snapshot = Config::deserializeConfigV3Snapshot(snapshotBlob.bytes.data(), snapshotBlob.bytes.size());
    assert(snapshot.schemaVersion == snapshotBlob.schemaVersion);
    assert(snapshot.snapshotVersion == snapshotBlob.snapshotVersion);
    const auto serviceModeKeyId = Config::tryKeyIdForPath("service.mode");
    assert(serviceModeKeyId.has_value());
    const auto* serviceMode = FindSnapshotEntry(snapshot, *serviceModeKeyId);
    assert(serviceMode != nullptr);
    assert(Config::getValue<std::string>(serviceMode->value) == "full");

    const auto autoModeKeyId = Config::tryKeyIdForPath("service.auto_mode");
    assert(autoModeKeyId.has_value());
    const auto payload = MakePatchPayload({Config::ConfigTlvEntry{*autoModeKeyId, Config::ConfigValueType::Bool, "false"}});
    auto chunk = MakeSingleChunk(7, payload);

    const auto applied = runtime.ApplyTlvChunk(chunk);
    assert(applied.status == Ipc::IpcStatusCode::Ok);
    assert(applied.completed);
    assert(applied.changedCount == 1);
    assert(!applied.desiredServiceConfig.autoMode);
    assert(HasAction(applied, Service::ConfigApplyActionKind::ServicePolicy));
    assert(!HasAction(applied, Service::ConfigApplyActionKind::PipelineRuntime));

    const auto changedCatalogBlob = runtime.BuildCatalogV3Blob();
    const auto changedSnapshotBlob = runtime.BuildSnapshotV3Blob();
    assert(changedCatalogBlob.schemaVersion == catalogBlob.schemaVersion);
    assert(changedSnapshotBlob.bytes != snapshotBlob.bytes);
    assert(changedSnapshotBlob.snapshotVersion != snapshotBlob.snapshotVersion);
    assert(changedSnapshotBlob.checksum != snapshotBlob.checksum);
    const auto changedSnapshot = Config::deserializeConfigV3Snapshot(changedSnapshotBlob.bytes.data(), changedSnapshotBlob.bytes.size());
    const auto* autoMode = FindSnapshotEntry(changedSnapshot, *autoModeKeyId);
    assert(autoMode != nullptr);
    assert(!Config::getValue<bool>(autoMode->value));

    const auto beforeRejectSnapshotBlob = runtime.BuildSnapshotV3Blob();
    auto invalidChunk = MakeSingleChunk(8, payload);
    invalidChunk.bytes[0] ^= 0xFF;
    const auto rejected = runtime.ApplyTlvChunk(invalidChunk);
    assert(rejected.status == Ipc::IpcStatusCode::InvalidRequest);
    assert(!runtime.ServiceState().autoMode);
    const auto afterRejectSnapshotBlob = runtime.BuildSnapshotV3Blob();
    assert(afterRejectSnapshotBlob.bytes == beforeRejectSnapshotBlob.bytes);
    assert(afterRejectSnapshotBlob.snapshotVersion == beforeRejectSnapshotBlob.snapshotVersion);
    assert(afterRejectSnapshotBlob.checksum == beforeRejectSnapshotBlob.checksum);

    const auto touchStepKeyId = Config::tryKeyIdForPath("touch.signal_cond.baseline_no_finger_max_step");
    assert(touchStepKeyId.has_value());
    const auto touchPayload = MakePatchPayload({Config::ConfigTlvEntry{*touchStepKeyId, Config::ConfigValueType::Int32, "513"}});
    const auto touchApplied = runtime.ApplyTlvChunk(MakeSingleChunk(9, touchPayload));
    assert(touchApplied.status == Ipc::IpcStatusCode::Ok);
    assert(touchApplied.completed);
    assert(touchApplied.changedCount == 1);
    assert(HasAction(touchApplied, Service::ConfigApplyActionKind::PipelineRuntime));

    const auto iirMaxKeyId = Config::tryKeyIdForPath("stylus.sp.iir_max_coef");
    const auto iirLowHoverKeyId = Config::tryKeyIdForPath("stylus.sp.iir_coef_low_hover");
    assert(iirMaxKeyId.has_value());
    assert(iirLowHoverKeyId.has_value());
    const auto beforeIirReject = runtime.BuildSnapshotV3Blob();
    const auto invalidIirPayload = MakePatchPayload({
        Config::ConfigTlvEntry{*iirMaxKeyId, Config::ConfigValueType::Int32, "1"},
        Config::ConfigTlvEntry{*iirLowHoverKeyId, Config::ConfigValueType::Int32, "2"},
    });
    const auto iirRejected = runtime.ApplyTlvChunk(MakeSingleChunk(10, invalidIirPayload));
    assert(iirRejected.status == Ipc::IpcStatusCode::InvalidRequest);
    assert(HasFailedTargetResult(iirRejected, "PipelineConfigTarget"));
    const auto afterIirReject = runtime.BuildSnapshotV3Blob();
    assert(afterIirReject.bytes == beforeIirReject.bytes);
    assert(afterIirReject.snapshotVersion == beforeIirReject.snapshotVersion);
    assert(afterIirReject.checksum == beforeIirReject.checksum);

    Service::ConfigRuntime rejectingRuntime;
    rejectingRuntime.RegisterConfigTarget(std::make_unique<RejectAutoModeTarget>());
    assert(rejectingRuntime.Initialize("", [](const Config::ConfigStore&) { return true; }));
    const auto rejectingBefore = rejectingRuntime.BuildSnapshotV3Blob();
    const auto targetRejected = rejectingRuntime.ApplyTlvChunk(MakeSingleChunk(11, payload));
    assert(targetRejected.status == Ipc::IpcStatusCode::InvalidRequest);
    assert(HasFailedTargetResult(targetRejected, "RejectAutoModeTarget"));
    assert(rejectingRuntime.ServiceState().autoMode);
    const auto rejectingAfter = rejectingRuntime.BuildSnapshotV3Blob();
    assert(rejectingAfter.bytes == rejectingBefore.bytes);
    assert(rejectingAfter.snapshotVersion == rejectingBefore.snapshotVersion);
    assert(rejectingAfter.checksum == rejectingBefore.checksum);

    return 0;
}
