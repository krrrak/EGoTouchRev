#include "vhf/VhfReporterStylusPacketHelper.h"
#include "TestRequire.h"

#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using DeviceTests::Require;
using Solvers::HeatmapFrame;

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
    return frame;
}

HeatmapFrame MakeInvalidOutputFrameWithStaleLegacyValidData() {
    HeatmapFrame frame{};
    return frame;
}

VhfStylusPacket::Config MakeDefaultStylusPacketConfig() {
    VhfStylusPacket::Config config;
    config.sensorRows = 40;
    config.sensorCols = 60;
    config.emitWhenInvalid = true;
    return config;
}

void TestStylusPacketHelperBuildsValidPacketFromOutput() {
    auto frame = MakeOutputDrivenStylusFrame();
    const auto packet = VhfStylusPacket::Build(
        frame.stylus, MakeDefaultStylusPacketConfig());

    Require(packet.valid,
            "valid output should build a stylus packet");
    Require(packet.length == 13,
            "VHF-built stylus packet should use 13-byte report length");
    Require(packet.bytes[0] == 0x08,
            "VHF-built stylus packet should keep stylus report id");
    Require(packet.bytes[1] == 0x21,
            "VHF-built valid stylus packet should encode TipSwitch and InRange");
    Require(ReadU16Le(packet, 3) == 7200,
            "VHF-built stylus packet should map output Y into HID X");
    Require(ReadU16Le(packet, 5) == 20480,
            "VHF-built stylus packet should map output X into HID Y");
    Require(ReadU16Le(packet, 7) == 321,
            "VHF-built stylus packet should encode output pressure");
    Require(ReadI16Le(packet, 9) == 7,
            "VHF-built stylus packet should encode output tiltX in degrees (scaled down by 100)");
    Require(ReadI16Le(packet, 11) == -3,
            "VHF-built stylus packet should encode output tiltY in degrees (scaled down by 100)");
    Require(VhfStylusPacket::ExtractPenState(packet) == 0x21,
            "helper pen state extraction should match raw packet");
}

void TestStylusPacketHelperBuildsInvalidZeroStatePacketFromInvalidOutputWhenEnabled() {
    auto frame = MakeInvalidOutputFrameWithStaleLegacyValidData();
    auto config = MakeDefaultStylusPacketConfig();
    config.emitWhenInvalid = true;
    const auto packet = VhfStylusPacket::Build(frame.stylus, config);

    Require(packet.valid,
            "invalid output should still build a packet when emitWhenInvalid is enabled");
    Require(packet.length == 13,
            "invalid output should build a 13-byte packet");
    Require(packet.bytes[1] == 0,
            "invalid output should clear stylus state bits");
    Require(ReadU16Le(packet, 3) == 0,
            "invalid output should clear HID X");
    Require(ReadU16Le(packet, 5) == 0,
            "invalid output should clear HID Y");
    Require(ReadU16Le(packet, 7) == 0,
            "invalid output should clear pressure");
    Require(VhfStylusPacket::ExtractPenState(packet) == 0,
            "invalid output should decode neutral pen state");
}

void TestStylusPacketHelperSuppressesInvalidPacketFromInvalidOutputWhenDisabled() {
    auto frame = MakeInvalidOutputFrameWithStaleLegacyValidData();
    auto config = MakeDefaultStylusPacketConfig();
    config.emitWhenInvalid = false;
    const auto packet = VhfStylusPacket::Build(frame.stylus, config);

    Require(!packet.valid,
            "invalid output should stay suppressed when emitWhenInvalid is disabled");
    Require(packet.length == 13,
            "suppressed invalid packet should preserve HID report length");
    Require(VhfStylusPacket::ExtractPenState(packet) == 0,
            "suppressed invalid packet should decode neutral pen state");
}

void TestBarrelButtonAndInRangeBits() {
    auto frame = MakeOutputDrivenStylusFrame();
    auto config = MakeDefaultStylusPacketConfig();
    config.barrelButton = true;

    auto packet = VhfStylusPacket::Build(frame.stylus, config);
    Require(packet.bytes[1] == 0x23,
            "barrel button should add barrel switch bit without clearing tip/in-range bits");

    frame.stylus.output.tipDown = false;
    frame.stylus.output.inRange = false;
    packet = VhfStylusPacket::Build(frame.stylus, config);
    Require(packet.valid, "point-valid output should still produce a report when out of range");
    Require(packet.bytes[1] == 0x02,
            "out-of-range point should clear InRange and TipSwitch while preserving barrel bit");
}

void TestPressureCoordinateAndTiltClamps() {
    auto frame = MakeOutputDrivenStylusFrame();
    frame.stylus.output.pressure = 9000;
    frame.stylus.output.point.x = -10.0f * 1024.0f;
    frame.stylus.output.point.y = 99.0f * 1024.0f;
    frame.stylus.output.point.tiltX = 12000;
    frame.stylus.output.point.tiltY = -12000;

    const auto packet = VhfStylusPacket::Build(frame.stylus, MakeDefaultStylusPacketConfig());
    Require(ReadU16Le(packet, 3) == 16000,
            "Y beyond active rows should clamp HID X to max");
    Require(ReadU16Le(packet, 5) == 25600,
            "negative X should clamp HID Y to max because X is inverted");
    Require(ReadU16Le(packet, 7) == 4095,
            "pressure should clamp to HID max 4095");
    Require(ReadI16Le(packet, 9) == 9000,
            "tiltX should clamp to +9000");
    Require(ReadI16Le(packet, 11) == -9000,
            "tiltY should clamp to -9000");
}

} // namespace

int main() {
    try {
        TestStylusPacketHelperBuildsValidPacketFromOutput();
        TestStylusPacketHelperBuildsInvalidZeroStatePacketFromInvalidOutputWhenEnabled();
        TestStylusPacketHelperSuppressesInvalidPacketFromInvalidOutputWhenDisabled();
        TestBarrelButtonAndInRangeBits();
        TestPressureCoordinateAndTiltClamps();
        std::cout << "[TEST] Device VHF reporter stylus packet tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
