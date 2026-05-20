#include "SystemStateMonitor.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

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

std::vector<HANDLE> OpenAllNamedEventsForTest() {
    std::vector<HANDLE> handles;
    const auto& named_events = Host::SystemStateMonitor::NamedEventList();
    handles.reserve(Host::SystemStateMonitor::kEventCount);

    for (const wchar_t* event_name : named_events) {
        HANDLE event_handle = CreateEventW(nullptr, TRUE, FALSE, event_name);
        if (event_handle == nullptr) {
            for (HANDLE handle : handles) {
                CloseHandle(handle);
            }
            handles.clear();
            return handles;
        }
        handles.push_back(event_handle);
    }

    return handles;
}

void CloseHandles(std::vector<HANDLE>& handles) {
    for (HANDLE handle : handles) {
        CloseHandle(handle);
    }
    handles.clear();
}

bool ExpectSequence(const std::vector<Host::SystemStateEventType>& expected,
                    const std::vector<Host::SystemStateEventType>& actual) {
    if (actual.size() < expected.size()) {
        return false;
    }

    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            return false;
        }
    }

    return true;
}

bool RunNamedEventSequenceTest() {
    using namespace std::chrono_literals;

    Host::SystemStateMonitor monitor;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Host::SystemStateEventType> observed;

    const bool started = monitor.Start([&](const Host::SystemStateEvent& event) {
        {
            std::lock_guard<std::mutex> lock(mu);
            observed.push_back(event.type);
        }
        cv.notify_all();
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed in sequence test.\n";
        return false;
    }

    ResetAllNamedEventsBestEffort();

    // Give worker thread a short warm-up window before injecting events.
    std::this_thread::sleep_for(80ms);

    const std::vector<std::pair<Host::SystemStateNamedEventId, Host::SystemStateEventType>> script = {
        {Host::SystemStateNamedEventId::MonitorConsoleDisplayOff, Host::SystemStateEventType::DisplayOff},
        {Host::SystemStateNamedEventId::MonitorConsoleDisplayOn, Host::SystemStateEventType::DisplayOn},
        {Host::SystemStateNamedEventId::MonitorLidOff, Host::SystemStateEventType::LidOff},
        {Host::SystemStateNamedEventId::PbtApmResumeAutomatic, Host::SystemStateEventType::ResumeAutomatic},
    };

    for (const auto& step : script) {
        if (!Host::SystemStateMonitor::SignalNamedEvent(step.first)) {
            monitor.Stop();
            std::cerr << "[TEST] Failed to signal named event in sequence test.\n";
            return false;
        }
        std::this_thread::sleep_for(80ms);
    }

    const std::vector<Host::SystemStateEventType> expected = {
        Host::SystemStateEventType::DisplayOff,
        Host::SystemStateEventType::DisplayOn,
        Host::SystemStateEventType::LidOff,
        Host::SystemStateEventType::ResumeAutomatic,
    };

    {
        std::unique_lock<std::mutex> lock(mu);
        const bool received_all = cv.wait_for(lock, 2s, [&] {
            return observed.size() >= expected.size();
        });
        if (!received_all) {
            monitor.Stop();
            std::cerr << "[TEST] Timeout waiting for sequence events. observed=" << observed.size() << "\n";
            return false;
        }
    }

    monitor.Stop();

    if (!ExpectSequence(expected, observed)) {
        std::cerr << "[TEST] Event sequence mismatch.\n";
        for (std::size_t i = 0; i < observed.size(); ++i) {
            std::cerr << "  observed[" << i << "]=" << Host::ToString(observed[i]) << "\n";
        }
        return false;
    }

    return true;
}

bool RunDisplayBurstCoalescingTest() {
    using namespace std::chrono_literals;

    auto event_handles = OpenAllNamedEventsForTest();
    if (event_handles.size() != Host::SystemStateMonitor::kEventCount) {
        std::cerr << "[TEST] Failed to create named events in display burst coalescing test.\n";
        return false;
    }

    ResetAllNamedEventsBestEffort();

    SetEvent(event_handles[Host::ToIndex(Host::SystemStateNamedEventId::MonitorConsoleDisplayOff)]);
    SetEvent(event_handles[Host::ToIndex(Host::SystemStateNamedEventId::MonitorConsoleDisplayOn)]);

    Host::SystemStateMonitor monitor;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Host::SystemStateEventType> observed;

    const bool started = monitor.Start([&](const Host::SystemStateEvent& event) {
        {
            std::lock_guard<std::mutex> lock(mu);
            observed.push_back(event.type);
        }
        cv.notify_all();
    });

    if (!started) {
        CloseHandles(event_handles);
        std::cerr << "[TEST] SystemStateMonitor start failed in display burst coalescing test.\n";
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(mu);
        const bool received_event = cv.wait_for(lock, 2s, [&] {
            return !observed.empty();
        });
        if (!received_event) {
            monitor.Stop();
            CloseHandles(event_handles);
            std::cerr << "[TEST] Timeout waiting for display burst event.\n";
            return false;
        }
    }

    std::this_thread::sleep_for(150ms);
    monitor.Stop();
    CloseHandles(event_handles);

    const std::vector<Host::SystemStateEventType> expected = {
        Host::SystemStateEventType::DisplayOn,
    };

    if (!ExpectSequence(expected, observed) || observed.size() != expected.size()) {
        std::cerr << "[TEST] Display burst coalescing mismatch.\n";
        for (std::size_t i = 0; i < observed.size(); ++i) {
            std::cerr << "  observed[" << i << "]=" << Host::ToString(observed[i]) << "\n";
        }
        return false;
    }

    return true;
}

bool RunCallbackExceptionContainmentTest() {
    using namespace std::chrono_literals;

    Host::SystemStateMonitor monitor;

    std::atomic<int> callback_count{0};
    std::mutex mu;
    std::condition_variable cv;

    const bool started = monitor.Start([&](const Host::SystemStateEvent&) {
        const int n = callback_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (n == 1) {
            throw std::runtime_error("intentional callback failure");
        }

        {
            std::lock_guard<std::mutex> lock(mu);
        }
        cv.notify_all();
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed in exception containment test.\n";
        return false;
    }

    ResetAllNamedEventsBestEffort();
    std::this_thread::sleep_for(80ms);

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
        const bool received_after_throw = cv.wait_for(lock, 2s, [&] {
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
    return true;
}

bool RunCallbackReentrantStopTest() {
    using namespace std::chrono_literals;

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

    ResetAllNamedEventsBestEffort();
    std::this_thread::sleep_for(80ms);

    if (!Host::SystemStateMonitor::SignalNamedEvent(Host::SystemStateNamedEventId::MonitorPowerOn)) {
        monitor.Stop();
        std::cerr << "[TEST] Failed signal in reentrant Stop test.\n";
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(mu);
        const bool callback_observed = cv.wait_for(lock, 2s, [&] {
            return callback_count.load(std::memory_order_acquire) >= 1;
        });
        if (!callback_observed) {
            monitor.Stop();
            std::cerr << "[TEST] Timeout waiting for callback in reentrant Stop test.\n";
            return false;
        }
    }

    // External Stop() should complete join/cleanup when callback called Stop() from worker thread.
    monitor.Stop();

    if (monitor.IsRunning()) {
        std::cerr << "[TEST] Monitor still running after reentrant Stop test.\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!RunNamedEventSequenceTest()) {
        return 1;
    }

    if (!RunDisplayBurstCoalescingTest()) {
        return 2;
    }

    if (!RunCallbackExceptionContainmentTest()) {
        return 3;
    }

    if (!RunCallbackReentrantStopTest()) {
        return 4;
    }

    std::cout << "[TEST] SystemStateMonitor named-event tests passed.\n";
    return 0;
}
