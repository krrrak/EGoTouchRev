#include "runtime/DeviceRuntime.h"

#include <chrono>
#include <iostream>
#include <thread>

namespace {

RuntimePolicyEvent MakePolicyEvent(RuntimePolicyEvent::Type type) {
    RuntimePolicyEvent event{};
    event.type = type;
    event.timestamp = std::chrono::system_clock::now();
    return event;
}

bool WaitForState(DeviceRuntime& runtime,
                  workerState expected,
                  std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (runtime.GetSnapshot().state == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return runtime.GetSnapshot().state == expected;
}

DeviceRuntime MakeRuntimeWithMissingHardware() {
    return DeviceRuntime(
        L"\\\\?\\EGoTouchMissingMaster",
        L"\\\\?\\EGoTouchMissingSlave",
        L"\\\\?\\EGoTouchMissingInterrupt");
}

bool RunResumeAfterSystemSuspendForcesReadyFromRecoverTest() {
    using namespace std::chrono_literals;

    auto runtime = MakeRuntimeWithMissingHardware();
    runtime.SetAutoMode(true);

    if (!runtime.Start()) {
        std::cerr << "[TEST] Failed to start runtime.\n";
        return false;
    }

    if (!WaitForState(runtime, workerState::recover, 2s)) {
        const auto snapshot = runtime.GetSnapshot();
        runtime.Stop();
        std::cerr << "[TEST] Runtime did not enter recover with missing hardware; state="
                  << ToString(snapshot.state) << "\n";
        return false;
    }

    runtime.SetAutoMode(false);
    runtime.IngestPolicyEvent(MakePolicyEvent(RuntimePolicyEvent::Type::Suspend));
    runtime.IngestPolicyEvent(MakePolicyEvent(RuntimePolicyEvent::Type::ResumeAutomatic));

    if (!WaitForState(runtime, workerState::ready, 2s)) {
        const auto snapshot = runtime.GetSnapshot();
        runtime.Stop();
        std::cerr << "[TEST] Resume after system suspend did not force ready; state="
                  << ToString(snapshot.state) << "\n";
        return false;
    }

    runtime.Stop();
    return true;
}

bool RunResumeWithoutSystemSuspendKeepsRecoverStateTest() {
    using namespace std::chrono_literals;

    auto runtime = MakeRuntimeWithMissingHardware();
    runtime.SetAutoMode(true);

    if (!runtime.Start()) {
        std::cerr << "[TEST] Failed to start runtime.\n";
        return false;
    }

    if (!WaitForState(runtime, workerState::recover, 2s)) {
        const auto snapshot = runtime.GetSnapshot();
        runtime.Stop();
        std::cerr << "[TEST] Runtime did not enter recover with missing hardware; state="
                  << ToString(snapshot.state) << "\n";
        return false;
    }

    runtime.SetAutoMode(false);
    runtime.IngestPolicyEvent(MakePolicyEvent(RuntimePolicyEvent::Type::ResumeAutomatic));
    std::this_thread::sleep_for(250ms);

    const auto snapshot = runtime.GetSnapshot();
    runtime.Stop();

    if (snapshot.state != workerState::recover) {
        std::cerr << "[TEST] Resume without system suspend unexpectedly changed state to "
                  << ToString(snapshot.state) << "\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!RunResumeAfterSystemSuspendForcesReadyFromRecoverTest()) {
        return 1;
    }

    if (!RunResumeWithoutSystemSuspendKeepsRecoverStateTest()) {
        return 2;
    }

    std::cout << "[TEST] DeviceRuntime system power policy tests passed.\n";
    return 0;
}
