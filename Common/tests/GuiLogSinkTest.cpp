#include "GuiLogSink.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestPushRawDrainAndClear() {
    auto sink = Common::GuiLogSink::Instance();
    sink->SetNotifyEvent(nullptr);
    sink->Clear();

    sink->PushRaw("line one");
    sink->PushRaw("line two");

    auto lines = sink->GetLines();
    Require(lines.size() == 2, "PushRaw should append to all buffered lines");
    Require(lines[0] == "line one" && lines[1] == "line two", "PushRaw should preserve insertion order");

    auto pending = sink->DrainNewLines();
    Require(pending.size() == 2, "DrainNewLines should return pending lines");
    Require(pending[0] == "line one" && pending[1] == "line two", "DrainNewLines should preserve pending order");
    Require(sink->DrainNewLines().empty(), "DrainNewLines should clear pending lines after draining");

    sink->Clear();
    Require(sink->GetLines().empty(), "Clear should remove buffered lines");
    Require(sink->DrainNewLines().empty(), "Clear should remove pending lines");
}

void TestMaxLineTrimming() {
    auto sink = Common::GuiLogSink::Instance();
    sink->SetNotifyEvent(nullptr);
    sink->Clear();

    for (int i = 0; i < Common::GuiLogSink::kMaxLines + 5; ++i) {
        sink->PushRaw("line " + std::to_string(i));
    }

    auto lines = sink->GetLines();
    auto pending = sink->DrainNewLines();

    Require(lines.size() == Common::GuiLogSink::kMaxLines, "GetLines should be capped at kMaxLines");
    Require(pending.size() == Common::GuiLogSink::kMaxLines, "Pending lines should be capped at kMaxLines");
    Require(lines.front() == "line 5", "Line trimming should discard the oldest full-buffer entries");
    Require(lines.back() == "line 2004", "Line trimming should keep the newest full-buffer entry");
    Require(pending.front() == "line 5", "Line trimming should discard the oldest pending entries");
    Require(pending.back() == "line 2004", "Line trimming should keep the newest pending entry");

    sink->Clear();
}

} // namespace

int main() {
    try {
        TestPushRawDrainAndClear();
        TestMaxLineTrimming();
        std::cout << "[TEST] CommonGuiLogSinkTest passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] CommonGuiLogSinkTest failed: " << ex.what() << '\n';
        return 1;
    }
}
