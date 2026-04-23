#include "vhf/VhfReporter.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

using Solvers::HeatmapFrame;
using Solvers::StylusPacketRoute;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

uint16_t ReadU16Le(const Solvers::StylusPacket& packet, size_t offset) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(packet.bytes[offset]) |
        (static_cast<uint16_t>(packet.bytes[offset + 1]) << 8));
}

int16_t ReadI16Le(const Solvers::StylusPacket& packet, size_t offset) {
    return static_cast<int16_t>(ReadU16Le(packet, offset));
}

HeatmapFrame MakeOutputDrivenStylusFrame() {
    HeatmapFrame frame{};

    frame.stylus.output.valid = true;
    frame.stylus.output.inRange = true;
    frame.stylus.output.tipDown = true;
    frame.stylus.output.pressure = 321;
    frame.stylus.output.point.valid = true;
    frame.stylus.output.point.x = 12.0f * 1024.0f;
    frame.stylus.output.point.y = 18.0f * 1024.0f;
    frame.stylus.output.point.tiltX = 7;
    frame.stylus.output.point.tiltY = -3;
    frame.stylus.output.point.pressure = frame.stylus.output.pressure;

    // Stale legacy mirrors should not affect reporter-side packet assembly.
    frame.stylus.packetRoute = StylusPacketRoute::InvalidZeroState;
    frame.stylus.point.valid = true;
    frame.stylus.point.x = 1.0f * 1024.0f;
    frame.stylus.point.y = 2.0f * 1024.0f;
    frame.stylus.point.tiltX = -9;
    frame.stylus.point.tiltY = 11;
    frame.stylus.pressure = 17;
    frame.stylus.tipSwitchActive = false;
    return frame;
}

HeatmapFrame MakeInvalidOutputFrameWithStaleLegacyValidData() {
    HeatmapFrame frame{};
    frame.stylus.packetRoute = StylusPacketRoute::Valid;
    frame.stylus.point.valid = true;
    frame.stylus.point.x = 20.0f * 1024.0f;
    frame.stylus.point.y = 30.0f * 1024.0f;
    frame.stylus.point.tiltX = 5;
    frame.stylus.point.tiltY = 6;
    frame.stylus.pressure = 777;
    frame.stylus.tipSwitchActive = true;
    return frame;
}

void TestDispatchStylusBuildsAndBackfillsValidPacketFromOutput() {
    VhfReporter reporter;
    reporter.SetEnabled(false);
    reporter.SetStylusPacketSensorRows(40);
    reporter.SetStylusPacketSensorCols(60);

    auto frame = MakeOutputDrivenStylusFrame();
    reporter.DispatchStylus(frame);

    Require(frame.stylus.packet.valid,
            "VHF should backfill a valid stylus packet before dispatch");
    Require(frame.stylus.packet.length == 13,
            "VHF-built stylus packet should use 13-byte report length");
    Require(frame.stylus.packet.bytes[0] == 0x08,
            "VHF-built stylus packet should keep stylus report id");
    Require(frame.stylus.packet.bytes[1] == 0x21,
            "VHF-built valid stylus packet should encode TipSwitch and InRange");
    Require(ReadU16Le(frame.stylus.packet, 3) == 7200,
            "VHF-built stylus packet should map output Y into HID X");
    Require(ReadU16Le(frame.stylus.packet, 5) == 20480,
            "VHF-built stylus packet should map output X into HID Y");
    Require(ReadU16Le(frame.stylus.packet, 7) == 321,
            "VHF-built stylus packet should encode output pressure");
    Require(ReadI16Le(frame.stylus.packet, 9) == 700,
            "VHF-built stylus packet should encode output tiltX in centidegrees");
    Require(ReadI16Le(frame.stylus.packet, 11) == -300,
            "VHF-built stylus packet should encode output tiltY in centidegrees");
    Require(frame.stylus.diag.vhfPenState == 0x21,
            "VHF should update diagnostics from the raw built packet state");
}

void TestDispatchStylusBuildsInvalidZeroStatePacketFromInvalidOutputWhenEnabled() {
    VhfReporter reporter;
    reporter.SetEnabled(false);
    reporter.SetStylusPacketEmitWhenInvalid(true);

    auto frame = MakeInvalidOutputFrameWithStaleLegacyValidData();
    reporter.DispatchStylus(frame);

    Require(frame.stylus.packet.valid,
            "invalid output should still build a packet when emitWhenInvalid is enabled");
    Require(frame.stylus.packet.length == 13,
            "invalid output should build a 13-byte packet");
    Require(frame.stylus.packet.bytes[1] == 0,
            "invalid output should clear stylus state bits");
    Require(ReadU16Le(frame.stylus.packet, 3) == 0,
            "invalid output should clear HID X");
    Require(ReadU16Le(frame.stylus.packet, 5) == 0,
            "invalid output should clear HID Y");
    Require(ReadU16Le(frame.stylus.packet, 7) == 0,
            "invalid output should clear pressure");
    Require(frame.stylus.diag.vhfPenState == 0,
            "invalid output should write neutral diagnostics state");
}

void TestDispatchStylusSuppressesInvalidPacketFromInvalidOutputWhenDisabled() {
    VhfReporter reporter;
    reporter.SetEnabled(false);
    reporter.SetStylusPacketEmitWhenInvalid(false);

    auto frame = MakeInvalidOutputFrameWithStaleLegacyValidData();
    reporter.DispatchStylus(frame);

    Require(!frame.stylus.packet.valid,
            "invalid output should stay suppressed when emitWhenInvalid is disabled");
    Require(frame.stylus.packet.length == 13,
            "suppressed invalid packet should preserve HID report length");
    Require(frame.stylus.diag.vhfPenState == 0,
            "suppressed invalid packet should keep neutral diagnostics state");
}

void TestDispatchStylusDoesNotWriteBackEraserTransform() {
    VhfReporter reporter;
    reporter.SetEnabled(false);
    reporter.SetEraserState(1);

    auto frame = MakeOutputDrivenStylusFrame();
    reporter.DispatchStylus(frame);

    Require(frame.stylus.packet.valid,
            "valid frame should still build a packet before any eraser transform");
    Require(frame.stylus.packet.bytes[1] == 0x21,
            "eraser post-transform must not be written back into frame.stylus.packet");
    Require(frame.stylus.diag.vhfPenState == 0x21,
            "diagnostics should reflect the raw packet, not the transformed write buffer");
}

void TestDispatchStylusBackfillsEvenWhenWriteDisabled() {
    VhfReporter reporter;
    reporter.SetEnabled(true);

    auto frame = MakeOutputDrivenStylusFrame();
    reporter.DispatchStylus(frame, false);

    Require(frame.stylus.packet.valid,
            "write-disabled dispatch should still backfill the stylus packet");
    Require(frame.stylus.packet.bytes[1] == 0x21,
            "write-disabled dispatch should still encode the raw pen state");
    Require(frame.stylus.diag.vhfPenState == 0x21,
            "write-disabled dispatch should still publish diagnostics state");
}

} // namespace

int main() {
    try {
        TestDispatchStylusBuildsAndBackfillsValidPacketFromOutput();
        TestDispatchStylusBuildsInvalidZeroStatePacketFromInvalidOutputWhenEnabled();
        TestDispatchStylusSuppressesInvalidPacketFromInvalidOutputWhenDisabled();
        TestDispatchStylusDoesNotWriteBackEraserTransform();
        TestDispatchStylusBackfillsEvenWhenWriteDisabled();
        std::cout << "[TEST] VHF reporter stylus packet tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
