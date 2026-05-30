#include "SystemStateMonitor.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr auto kWorkerWarmup = std::chrono::milliseconds(80);
constexpr auto kCallbackTimeout = std::chrono::seconds(2);
constexpr auto kNoExtraCallbackWindow = std::chrono::milliseconds(200);

void ResetNamedEventBestEffort(const wchar_t* event_name) {
    HANDLE event_handle = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
    if (event_handle == nullptr) {
        return;
    }
    ResetEvent(event_handle);
    CloseHandle(event_handle);
}

void ResetAllNamedEventsBestEffort() {
    const auto& named_events = Host::SystemStateMonitor::NamedEventList();
    for (const wchar_t* event_name : named_events) {
        ResetNamedEventBestEffort(event_name);
    }
}

struct TestEventHandles {
    std::vector<HANDLE> handles;

    ~TestEventHandles() {
        Close();
    }

    TestEventHandles(const TestEventHandles&) = delete;
    TestEventHandles& operator=(const TestEventHandles&) = delete;

    TestEventHandles() = default;

    bool OpenAll() {
        Close();
        const auto& named_events = Host::SystemStateMonitor::NamedEventList();
        handles.reserve(Host::SystemStateMonitor::kEventCount);

        for (const wchar_t* event_name : named_events) {
            HANDLE event_handle = CreateEventW(nullptr, TRUE, FALSE, event_name);
            if (event_handle == nullptr) {
                std::wcerr << L"[TEST] CreateEventW failed for " << event_name
                           << L" error=" << GetLastError() << L"\n";
                Close();
                return false;
            }
            handles.push_back(event_handle);
        }

        return handles.size() == Host::SystemStateMonitor::kEventCount;
    }

    void Close() noexcept {
        for (HANDLE handle : handles) {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
        }
        handles.clear();
    }

    bool Set(Host::SystemStateNamedEventId id) const noexcept {
        const std::size_t index = Host::ToIndex(id);
        if (index >= handles.size() || handles[index] == nullptr) {
            return false;
        }
        return SetEvent(handles[index]) != FALSE;
    }

    bool Reset(Host::SystemStateNamedEventId id) const noexcept {
        const std::size_t index = Host::ToIndex(id);
        if (index >= handles.size() || handles[index] == nullptr) {
            return false;
        }
        return ResetEvent(handles[index]) != FALSE;
    }
};

struct ObservedEvent {
    Host::SystemStateEventType type = Host::SystemStateEventType::Unknown;
    std::uint32_t raw_index = 0;
};

class EventRecorder {
public:
    void Record(const Host::SystemStateEvent& event) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            observed_.push_back({event.type, event.raw_index});
        }
        cv_.notify_all();
    }

    bool WaitForCount(std::size_t expected_count, std::chrono::milliseconds timeout = kCallbackTimeout) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, timeout, [&] {
            return observed_.size() >= expected_count;
        });
    }

    std::vector<ObservedEvent> Snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return observed_;
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return observed_.size();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<ObservedEvent> observed_;
};

void DumpObserved(const std::vector<ObservedEvent>& observed) {
    for (std::size_t i = 0; i < observed.size(); ++i) {
        std::cerr << "  observed[" << i << "]=" << Host::ToString(observed[i].type)
                  << " raw_index=" << observed[i].raw_index << "\n";
    }
}

bool ExpectTypes(const std::vector<Host::SystemStateEventType>& expected,
                 const std::vector<ObservedEvent>& actual,
                 const char* scenario) {
    if (actual.size() != expected.size()) {
        std::cerr << "[TEST] " << scenario << " count mismatch. expected=" << expected.size()
                  << " actual=" << actual.size() << "\n";
        DumpObserved(actual);
        return false;
    }

    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i].type) {
            std::cerr << "[TEST] " << scenario << " type mismatch at index " << i
                      << ". expected=" << Host::ToString(expected[i])
                      << " actual=" << Host::ToString(actual[i].type) << "\n";
            DumpObserved(actual);
            return false;
        }
    }

    return true;
}

bool SignalAndWaitForCount(Host::SystemStateMonitor& monitor,
                           EventRecorder& recorder,
                           Host::SystemStateNamedEventId id,
                           std::size_t expected_count,
                           const char* scenario) {
    if (!Host::SystemStateMonitor::SignalNamedEvent(id)) {
        monitor.Stop();
        std::cerr << "[TEST] Failed to signal named event in " << scenario << ". id="
                  << static_cast<int>(id) << "\n";
        return false;
    }

    if (!recorder.WaitForCount(expected_count)) {
        monitor.Stop();
        std::cerr << "[TEST] Timeout waiting for callback count " << expected_count
                  << " in " << scenario << ". observed=" << recorder.Size() << "\n";
        return false;
    }

    return true;
}

bool SignalAndExpectNoAdditionalCallback(Host::SystemStateMonitor& monitor,
                                         EventRecorder& recorder,
                                         Host::SystemStateNamedEventId id,
                                         std::size_t stable_count,
                                         const char* scenario) {
    if (!Host::SystemStateMonitor::SignalNamedEvent(id)) {
        monitor.Stop();
        std::cerr << "[TEST] Failed to signal duplicate named event in " << scenario << ". id="
                  << static_cast<int>(id) << "\n";
        return false;
    }

    if (recorder.WaitForCount(stable_count + 1, kNoExtraCallbackWindow)) {
        monitor.Stop();
        std::cerr << "[TEST] Unexpected duplicate callback in " << scenario
                  << ". observed=" << recorder.Size() << "\n";
        return false;
    }

    return true;
}

bool RunNamedEventInfrastructureTest() {
    if (Host::SystemStateMonitor::kEventCount != Host::kSystemStateNamedEventCount) {
        std::cerr << "[TEST] kEventCount does not match kSystemStateNamedEventCount.\n";
        return false;
    }

    const auto& specs = Host::SystemStateMonitor::NamedEventSpecs();
    const auto& names = Host::SystemStateMonitor::NamedEventList();
    for (std::size_t i = 0; i < Host::SystemStateMonitor::kEventCount; ++i) {
        if (specs[i].name == nullptr || specs[i].name[0] == L'\0' || names[i] == nullptr) {
            std::cerr << "[TEST] Empty named event spec at index " << i << ".\n";
            return false;
        }
        if (specs[i].name != names[i]) {
            std::cerr << "[TEST] NamedEventSpecs/NamedEventList mismatch at index " << i << ".\n";
            return false;
        }
    }

    TestEventHandles event_handles;
    if (!event_handles.OpenAll()) {
        std::cerr << "[TEST] Failed to create/open all named events in infrastructure test.\n";
        return false;
    }

    ResetAllNamedEventsBestEffort();
    if (!event_handles.Set(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn) ||
        WaitForSingleObject(event_handles.handles[Host::ToIndex(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn)], 0) != WAIT_OBJECT_0 ||
        !event_handles.Reset(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn) ||
        WaitForSingleObject(event_handles.handles[Host::ToIndex(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn)], 0) == WAIT_OBJECT_0) {
        std::cerr << "[TEST] Named event SetEvent/ResetEvent sanity check failed.\n";
        ResetAllNamedEventsBestEffort();
        return false;
    }

    ResetAllNamedEventsBestEffort();
    return true;
}

bool RunNamedEventSequenceTest() {
    using namespace std::chrono_literals;

    ResetAllNamedEventsBestEffort();

    Host::SystemStateMonitor monitor;
    EventRecorder recorder;

    const bool started = monitor.Start([&](const Host::SystemStateEvent& event) {
        recorder.Record(event);
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed in sequence test.\n";
        return false;
    }

    std::this_thread::sleep_for(kWorkerWarmup);

    const std::vector<std::pair<Host::SystemStateNamedEventId, Host::SystemStateEventType>> script = {
        {Host::SystemStateNamedEventId::MonitorConsoleDisplayOff, Host::SystemStateEventType::DisplayOff},
        {Host::SystemStateNamedEventId::MonitorConsoleDisplayOn, Host::SystemStateEventType::DisplayOn},
        {Host::SystemStateNamedEventId::MonitorLidOff, Host::SystemStateEventType::LidOff},
        {Host::SystemStateNamedEventId::PbtApmSuspend, Host::SystemStateEventType::Suspend},
        {Host::SystemStateNamedEventId::PbtApmResumeAutomatic, Host::SystemStateEventType::ResumeAutomatic},
        {Host::SystemStateNamedEventId::PbtApmResumeSuspend, Host::SystemStateEventType::ResumeAutomatic},
    };

    for (std::size_t i = 0; i < script.size(); ++i) {
        if (!SignalAndWaitForCount(monitor, recorder, script[i].first, i + 1, "sequence test")) {
            return false;
        }
        std::this_thread::sleep_for(30ms);
    }

    monitor.Stop();
    ResetAllNamedEventsBestEffort();

    std::vector<Host::SystemStateEventType> expected;
    expected.reserve(script.size());
    for (const auto& step : script) {
        expected.push_back(step.second);
    }

    return ExpectTypes(expected, recorder.Snapshot(), "sequence test");
}

bool RunAliasAndCanonicalBatchTest() {
    ResetAllNamedEventsBestEffort();

    TestEventHandles event_handles;
    if (!event_handles.OpenAll()) {
        std::cerr << "[TEST] Failed to create named events in alias/canonical batch test.\n";
        return false;
    }

    if (!event_handles.Set(Host::SystemStateNamedEventId::MonitorPowerOn) ||
        !event_handles.Set(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn) ||
        !event_handles.Set(Host::SystemStateNamedEventId::MonitorPowerOff) ||
        !event_handles.Set(Host::SystemStateNamedEventId::MonitorConsoleDisplayOff)) {
        std::cerr << "[TEST] Failed to pre-signal alias/canonical events.\n";
        ResetAllNamedEventsBestEffort();
        return false;
    }

    Host::SystemStateMonitor monitor;
    EventRecorder recorder;

    const bool started = monitor.Start([&](const Host::SystemStateEvent& event) {
        recorder.Record(event);
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed in alias/canonical batch test.\n";
        ResetAllNamedEventsBestEffort();
        return false;
    }

    if (!recorder.WaitForCount(1)) {
        monitor.Stop();
        ResetAllNamedEventsBestEffort();
        std::cerr << "[TEST] Timeout waiting for alias/canonical batch event. observed="
                  << recorder.Size() << "\n";
        return false;
    }

    if (recorder.WaitForCount(2, kNoExtraCallbackWindow)) {
        monitor.Stop();
        ResetAllNamedEventsBestEffort();
        std::cerr << "[TEST] Alias/canonical display burst emitted more than one normalized event.\n";
        DumpObserved(recorder.Snapshot());
        return false;
    }

    monitor.Stop();
    ResetAllNamedEventsBestEffort();

    const auto observed = recorder.Snapshot();
    const std::uint32_t canonical_on_index = static_cast<std::uint32_t>(Host::ToIndex(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn));
    if (!ExpectTypes({Host::SystemStateEventType::DisplayOn}, observed, "alias/canonical batch test")) {
        return false;
    }
    if (observed[0].raw_index != canonical_on_index) {
        std::cerr << "[TEST] Canonical DisplayOn raw_index did not win alias/canonical batch. expected="
                  << canonical_on_index << " actual=" << observed[0].raw_index << "\n";
        return false;
    }

    return true;
}

bool RunStateNormalizationDedupTest() {
    using namespace std::chrono_literals;

    ResetAllNamedEventsBestEffort();

    Host::SystemStateMonitor monitor;
    EventRecorder recorder;

    const bool started = monitor.Start([&](const Host::SystemStateEvent& event) {
        recorder.Record(event);
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed in state normalization test.\n";
        return false;
    }

    std::this_thread::sleep_for(kWorkerWarmup);

    if (!SignalAndWaitForCount(monitor, recorder, Host::SystemStateNamedEventId::MonitorConsoleDisplayOn, 1, "state normalization test") ||
        !SignalAndExpectNoAdditionalCallback(monitor, recorder, Host::SystemStateNamedEventId::MonitorPowerOn, 1, "duplicate DisplayOn alias test") ||
        !SignalAndWaitForCount(monitor, recorder, Host::SystemStateNamedEventId::MonitorConsoleDisplayOff, 2, "DisplayOff transition test") ||
        !SignalAndExpectNoAdditionalCallback(monitor, recorder, Host::SystemStateNamedEventId::MonitorPowerOff, 2, "duplicate DisplayOff alias test") ||
        !SignalAndWaitForCount(monitor, recorder, Host::SystemStateNamedEventId::MonitorLidOn, 3, "LidOn transition test") ||
        !SignalAndExpectNoAdditionalCallback(monitor, recorder, Host::SystemStateNamedEventId::MonitorLidOn, 3, "duplicate LidOn test") ||
        !SignalAndWaitForCount(monitor, recorder, Host::SystemStateNamedEventId::MonitorShutDown, 4, "Shutdown transition test") ||
        !SignalAndExpectNoAdditionalCallback(monitor, recorder, Host::SystemStateNamedEventId::MonitorShutDown, 4, "duplicate Shutdown test") ||
        !SignalAndWaitForCount(monitor, recorder, Host::SystemStateNamedEventId::PbtApmResumeAutomatic, 5, "first ResumeAutomatic test") ||
        !SignalAndWaitForCount(monitor, recorder, Host::SystemStateNamedEventId::PbtApmResumeSuspend, 6, "second ResumeAutomatic test")) {
        return false;
    }

    monitor.Stop();
    ResetAllNamedEventsBestEffort();

    const std::vector<Host::SystemStateEventType> expected = {
        Host::SystemStateEventType::DisplayOn,
        Host::SystemStateEventType::DisplayOff,
        Host::SystemStateEventType::LidOn,
        Host::SystemStateEventType::Shutdown,
        Host::SystemStateEventType::ResumeAutomatic,
        Host::SystemStateEventType::ResumeAutomatic,
    };

    return ExpectTypes(expected, recorder.Snapshot(), "state normalization test");
}

bool RunDisplayBurstCoalescingTest() {
    ResetAllNamedEventsBestEffort();

    TestEventHandles event_handles;
    if (!event_handles.OpenAll()) {
        std::cerr << "[TEST] Failed to create named events in display burst coalescing test.\n";
        return false;
    }

    if (!event_handles.Set(Host::SystemStateNamedEventId::MonitorConsoleDisplayOff) ||
        !event_handles.Set(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn)) {
        std::cerr << "[TEST] Failed to pre-signal display burst events.\n";
        ResetAllNamedEventsBestEffort();
        return false;
    }

    Host::SystemStateMonitor monitor;
    EventRecorder recorder;

    const bool started = monitor.Start([&](const Host::SystemStateEvent& event) {
        recorder.Record(event);
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed in display burst coalescing test.\n";
        ResetAllNamedEventsBestEffort();
        return false;
    }

    if (!recorder.WaitForCount(1)) {
        monitor.Stop();
        ResetAllNamedEventsBestEffort();
        std::cerr << "[TEST] Timeout waiting for display burst event.\n";
        return false;
    }

    if (recorder.WaitForCount(2, kNoExtraCallbackWindow)) {
        monitor.Stop();
        ResetAllNamedEventsBestEffort();
        std::cerr << "[TEST] Display burst emitted more than one event.\n";
        DumpObserved(recorder.Snapshot());
        return false;
    }

    monitor.Stop();
    ResetAllNamedEventsBestEffort();

    return ExpectTypes({Host::SystemStateEventType::DisplayOn}, recorder.Snapshot(), "display burst coalescing test");
}

bool RunCallbackExceptionContainmentTest() {
    using namespace std::chrono_literals;

    ResetAllNamedEventsBestEffort();

    Host::SystemStateMonitor monitor;

    std::atomic<int> callback_count{0};
    std::mutex mu;
    std::condition_variable cv;

    const bool started = monitor.Start([&](const Host::SystemStateEvent&) {
        const int n = callback_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (n == 1) {
            throw std::runtime_error("intentional callback failure");
        }

        cv.notify_all();
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed in exception containment test.\n";
        return false;
    }

    std::this_thread::sleep_for(kWorkerWarmup);

    if (!Host::SystemStateMonitor::SignalNamedEvent(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn)) {
        monitor.Stop();
        std::cerr << "[TEST] Failed first signal in exception containment test.\n";
        return false;
    }

    std::this_thread::sleep_for(30ms);

    if (!Host::SystemStateMonitor::SignalNamedEvent(Host::SystemStateNamedEventId::MonitorLidOn)) {
        monitor.Stop();
        std::cerr << "[TEST] Failed second signal in exception containment test.\n";
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(mu);
        const bool received_after_throw = cv.wait_for(lock, kCallbackTimeout, [&] {
            return callback_count.load(std::memory_order_acquire) >= 2;
        });
        if (!received_after_throw) {
            monitor.Stop();
            std::cerr << "[TEST] Monitor did not process callback after exception. count="
                      << callback_count.load(std::memory_order_acquire) << "\n";
            return false;
        }
    }

    monitor.Stop();
    ResetAllNamedEventsBestEffort();
    return true;
}

bool RunCallbackReentrantStopTest() {
    using namespace std::chrono_literals;

    ResetAllNamedEventsBestEffort();

    Host::SystemStateMonitor monitor;

    std::atomic<int> callback_count{0};
    std::mutex mu;
    std::condition_variable cv;

    const bool started = monitor.Start([&](const Host::SystemStateEvent&) {
        callback_count.fetch_add(1, std::memory_order_acq_rel);
        monitor.Stop();
        cv.notify_all();
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed in reentrant Stop test.\n";
        return false;
    }

    std::this_thread::sleep_for(kWorkerWarmup);

    if (!Host::SystemStateMonitor::SignalNamedEvent(Host::SystemStateNamedEventId::MonitorPowerOn)) {
        monitor.Stop();
        std::cerr << "[TEST] Failed signal in reentrant Stop test.\n";
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(mu);
        const bool callback_observed = cv.wait_for(lock, kCallbackTimeout, [&] {
            return callback_count.load(std::memory_order_acquire) >= 1;
        });
        if (!callback_observed) {
            monitor.Stop();
            std::cerr << "[TEST] Timeout waiting for callback in reentrant Stop test.\n";
            return false;
        }
    }

    monitor.Stop();
    ResetAllNamedEventsBestEffort();

    if (monitor.IsRunning()) {
        std::cerr << "[TEST] Monitor still running after reentrant Stop test.\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!RunNamedEventInfrastructureTest()) {
        return 1;
    }

    if (!RunNamedEventSequenceTest()) {
        return 2;
    }

    if (!RunAliasAndCanonicalBatchTest()) {
        return 3;
    }

    if (!RunStateNormalizationDedupTest()) {
        return 4;
    }

    if (!RunDisplayBurstCoalescingTest()) {
        return 5;
    }

    if (!RunCallbackExceptionContainmentTest()) {
        return 6;
    }

    if (!RunCallbackReentrantStopTest()) {
        return 7;
    }

    std::cout << "[TEST] SystemStateMonitor named-event tests passed.\n";
    return 0;
}
