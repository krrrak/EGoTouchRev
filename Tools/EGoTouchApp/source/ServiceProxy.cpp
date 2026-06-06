#include "ServiceProxy.h"
#include "ServiceProxyInternal.h"

namespace App {

TouchPipelineModuleEnableState CaptureTouchPipelineModuleEnableState(
    const Solvers::TouchPipeline& pipeline) {
    TouchPipelineModuleEnableState state{};
    state.baselineEnabled = pipeline.m_baseline.m_enabled;
    state.cmfEnabled = pipeline.m_cmf.m_enabled;
    state.gridIIREnabled = true;  // TODO: gridIIR removed; keep default
    state.trackerEnabled = pipeline.m_tracker.m_enabled;
    state.coordFilterEnabled = pipeline.m_coordFilter.m_enabled;
    state.gestureEnabled = pipeline.m_gesture.m_enabled;
    return state;
}

void ApplyTouchPipelineModuleEnableState(
    Solvers::TouchPipeline& pipeline,
    const TouchPipelineModuleEnableState& state) {
    const bool oldTrackerEnabled = pipeline.m_tracker.m_enabled;
    const bool oldGestureEnabled = pipeline.m_gesture.m_enabled;

    pipeline.m_baseline.m_enabled = state.baselineEnabled;
    pipeline.m_cmf.m_enabled = state.cmfEnabled;
    // gridIIREnabled not applicable (GridIIR disabled per project memory)
    pipeline.m_tracker.m_enabled = state.trackerEnabled;
    pipeline.m_coordFilter.m_enabled = state.coordFilterEnabled;
    pipeline.m_gesture.m_enabled = state.gestureEnabled;

    if (pipeline.m_tracker.m_enabled != oldTrackerEnabled) {
        pipeline.m_tracker.ClearLiveState();
    }
    if (pipeline.m_gesture.m_enabled != oldGestureEnabled) {
        pipeline.m_gesture.ClearLiveState();
    }
}

DvrRuntimeConfigSnapshot BuildRuntimeConfigSnapshotFromState(
    const ServiceRuntimeConfigState&,
    const AppRuntimeConfigState&,
    const Solvers::TouchPipeline&,
    const Solvers::StylusPipeline&) {
    return DvrRuntimeConfigSnapshot{};
}

DvrRuntimeConfigSnapshot ServiceProxy::CaptureRuntimeConfigSnapshot() const {
    return DvrRuntimeConfigSnapshot{};
}

} // namespace App
