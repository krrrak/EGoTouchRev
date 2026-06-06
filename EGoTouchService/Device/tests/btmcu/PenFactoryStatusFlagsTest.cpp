#include "btmcu/PenUsbTypes.h"
#include "TestRequire.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using DeviceTests::Require;
using EC = Himax::Pen::PenUsbEventCode;

uint16_t Apply(uint16_t flags, EC code, uint8_t payload) {
    return Himax::Pen::ApplyFactoryStatusFlagUpdate(flags, code, payload);
}

void TestSingleBitEvents() {
    Require(Apply(0, EC::PenAcStatus, 1) == 0x0001, "0x70 should set flags0 bit0");
    Require(Apply(0xFFFF, EC::PenAcStatus, 0) == 0xFFFE, "0x70 should clear flags0 bit0");

    Require(Apply(0, EC::PenConnStatus, 1) == 0x0002, "0x71 should set flags0 bit1");
    Require(Apply(0xFFFF, EC::PenConnStatus, 0) == 0xFFFD, "0x71 should clear flags0 bit1");

    Require(Apply(0, EC::PenTouchMode, 1) == 0x0100, "0x75 should set flags1 bit0");
    Require(Apply(0xFFFF, EC::PenTouchMode, 0) == 0xFEFF, "0x75 should clear flags1 bit0");

    Require(Apply(0, EC::PenGlobalPreventMode, 1) == 0x0200, "0x76 should set flags1 bit1");
    Require(Apply(0xFFFF, EC::PenGlobalPreventMode, 0) == 0xFDFF, "0x76 should clear flags1 bit1");

    Require(Apply(0, EC::PenHolster, 1) == 0x0800, "0x78 should set flags1 bit3");
    Require(Apply(0xFFFF, EC::PenHolster, 0) == 0xF7FF, "0x78 should clear flags1 bit3");
}

void TestPenCurStatusMapping() {
    Require(Apply(0xFFFF, EC::PenCurStatus, 1) == 0xFFF3, "0x72 payload 1 should clear bits2/3");
    Require(Apply(0, EC::PenCurStatus, 2) == 0x0004, "0x72 payload 2 should set bit2");
    Require(Apply(0, EC::PenCurStatus, 3) == 0x0008, "0x72 payload 3 should set bit3");
    Require(Apply(0x000C, EC::PenCurStatus, 0xFF) == 0x000C, "unknown 0x72 payload should preserve bits2/3");
}

void TestPenTypeAndRotationMapping() {
    Require(Apply(0, EC::PenTypeInfo, 0) == 0x0000, "0x73 payload 0 should clear type bits");
    Require(Apply(0, EC::PenTypeInfo, 1) == 0x0010, "0x73 payload 1 should map to bit4");
    Require(Apply(0, EC::PenTypeInfo, 2) == 0x0020, "0x73 payload 2 should map to bit5");
    Require(Apply(0, EC::PenTypeInfo, 3) == 0x0030, "0x73 should copy low two payload bits to bits4/5");
    Require(Apply(0xFFFF, EC::PenTypeInfo, 0) == 0xFFCF, "0x73 payload 0 should clear bits4/5");

    Require(Apply(0x00C0, EC::PenRotateAngle, 2) == 0, "0x74 payload 2 should clear bits6/7");
    Require(Apply(0, EC::PenRotateAngle, 4) == 0x0080, "0x74 payload 4 should set bit7");
    Require(Apply(0, EC::PenRotateAngle, 1) == 0x0040, "0x74 payload 1 should map through shifted low bits");
    Require(Apply(0, EC::PenRotateAngle, 3) == 0x00C0, "0x74 payload 3 should map through shifted low bits");
}

void TestAffectedEventTable() {
    const std::array<EC, 8> affected{
        EC::PenAcStatus,
        EC::PenConnStatus,
        EC::PenCurStatus,
        EC::PenTypeInfo,
        EC::PenRotateAngle,
        EC::PenTouchMode,
        EC::PenGlobalPreventMode,
        EC::PenHolster,
    };
    for (const auto code : affected) {
        Require(Himax::Pen::FactoryStatusFlagsAffected(code),
                "status-affecting factory event should be marked affected");
    }

    const std::array<EC, 8> unaffected{
        EC::PenModule,
        EC::PenCurrentFunc,
        EC::PenUnknown6F,
        EC::PenScreenStatus,
        EC::PenFreqJump,
        EC::PenRepParam,
        EC::PenGlobalAnnotation,
        EC::EraserToggle,
    };
    for (const auto code : unaffected) {
        Require(!Himax::Pen::FactoryStatusFlagsAffected(code),
                "non-status factory event should not be marked affected");
        Require(Apply(0x1234, code, 0xFF) == 0x1234,
                "non-status factory event should preserve existing flags");
    }
}

void TestSequenceUpdatesDoNotPolluteUnrelatedFields() {
    uint16_t flags = 0;
    flags = Apply(flags, EC::PenAcStatus, 1);
    flags = Apply(flags, EC::PenConnStatus, 1);
    flags = Apply(flags, EC::PenCurStatus, 3);
    flags = Apply(flags, EC::PenTypeInfo, 2);
    flags = Apply(flags, EC::PenRotateAngle, 1);
    flags = Apply(flags, EC::PenTouchMode, 1);
    flags = Apply(flags, EC::PenGlobalPreventMode, 1);
    flags = Apply(flags, EC::PenHolster, 1);
    Require(flags == 0x0B6B,
            "composite factory status flags should combine independent bit fields");

    flags = Apply(flags, EC::PenCurStatus, 1);
    Require(flags == 0x0B63,
            "writing mode should clear only current mode bits");
    flags = Apply(flags, EC::PenTypeInfo, 0);
    Require(flags == 0x0B43,
            "type update should clear only pen type bits");
}

} // namespace

int main() {
    try {
        TestSingleBitEvents();
        TestPenCurStatusMapping();
        TestPenTypeAndRotationMapping();
        TestAffectedEventTable();
        TestSequenceUpdatesDoNotPolluteUnrelatedFields();
        std::cout << "[TEST] Device Pen factory status flags tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
