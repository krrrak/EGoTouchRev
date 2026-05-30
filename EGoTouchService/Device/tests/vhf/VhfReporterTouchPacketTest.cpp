#include "vhf/VhfReporterTouchPacketHelper.h"
#include "TestRequire.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using DeviceTests::Require;
using Solvers::TouchContact;
using Solvers::TouchReportMove;
using Solvers::TouchReportUp;

TouchContact MakeContact(int id, int reportEvent, bool reported = true) {
    TouchContact contact;
    contact.id = id;
    contact.x = static_cast<float>(id);
    contact.y = static_cast<float>(id);
    contact.isReported = reported;
    contact.reportEvent = reportEvent;
    return contact;
}

uint8_t ContactState(const VhfTouchPacket::TouchPackets& packets, size_t contactIndex) {
    const auto& packet = packets[contactIndex / VhfTouchPacket::kContactsPerPacket];
    const size_t slot = contactIndex % VhfTouchPacket::kContactsPerPacket;
    return packet.bytes[VhfTouchPacket::kTouchPayloadOffset +
                        slot * VhfTouchPacket::kTouchContactStride];
}

uint8_t ContactId(const VhfTouchPacket::TouchPackets& packets, size_t contactIndex) {
    const auto& packet = packets[contactIndex / VhfTouchPacket::kContactsPerPacket];
    const size_t slot = contactIndex % VhfTouchPacket::kContactsPerPacket;
    return packet.bytes[VhfTouchPacket::kTouchPayloadOffset +
                        slot * VhfTouchPacket::kTouchContactStride + 1];
}

uint16_t ContactY(const VhfTouchPacket::TouchPackets& packets, size_t contactIndex) {
    const auto& packet = packets[contactIndex / VhfTouchPacket::kContactsPerPacket];
    const size_t slot = contactIndex % VhfTouchPacket::kContactsPerPacket;
    const size_t offset = VhfTouchPacket::kTouchPayloadOffset +
                          slot * VhfTouchPacket::kTouchContactStride + 2;
    return static_cast<uint16_t>(packet.bytes[offset] |
                                 (static_cast<uint16_t>(packet.bytes[offset + 1]) << 8));
}

uint16_t ContactX(const VhfTouchPacket::TouchPackets& packets, size_t contactIndex) {
    const auto& packet = packets[contactIndex / VhfTouchPacket::kContactsPerPacket];
    const size_t slot = contactIndex % VhfTouchPacket::kContactsPerPacket;
    const size_t offset = VhfTouchPacket::kTouchPayloadOffset +
                          slot * VhfTouchPacket::kTouchContactStride + 4;
    return static_cast<uint16_t>(packet.bytes[offset] |
                                 (static_cast<uint16_t>(packet.bytes[offset + 1]) << 8));
}

uint8_t ContactCount(const VhfTouchPacket::TouchPackets& packets) {
    return packets[0].bytes[VhfTouchPacket::kTouchContactCountOffset];
}

void TestLateUpPreemptsTenthMove() {
    std::vector<TouchContact> contacts;
    for (int id = 1; id <= 10; ++id) {
        contacts.push_back(MakeContact(id, TouchReportMove));
    }
    contacts.push_back(MakeContact(42, TouchReportUp));

    const auto packets = VhfTouchPacket::Build(contacts, false);

    Require(packets[0].valid && packets[1].valid,
            "ten packed contacts should use both touch packets");
    Require(ContactCount(packets) == 10,
            "late Up priority case should still report exactly ten contacts");
    Require(ContactState(packets, 0) == 0x02 && ContactId(packets, 0) == 42,
            "late Up contact should be packed first instead of being dropped");
    Require(ContactId(packets, 9) == 9,
            "the tenth active contact should be dropped before the late Up");
}

void TestNineMovesAndOneUpAreAllPacked() {
    std::vector<TouchContact> contacts;
    for (int id = 1; id <= 9; ++id) {
        contacts.push_back(MakeContact(id, TouchReportMove));
    }
    contacts.push_back(MakeContact(99, TouchReportUp));

    const auto packets = VhfTouchPacket::Build(contacts, false);

    Require(ContactCount(packets) == 10,
            "nine moves plus one Up should all be packed");
    Require(ContactState(packets, 0) == 0x02 && ContactId(packets, 0) == 99,
            "single Up should still be prioritized");
    Require(ContactId(packets, 9) == 9,
            "all nine move contacts should remain packed after the Up");
}

void TestMultipleUpsPreemptMovesUnderCapacityPressure() {
    std::vector<TouchContact> contacts;
    for (int id = 1; id <= 12; ++id) {
        contacts.push_back(MakeContact(id, TouchReportMove));
    }
    contacts.push_back(MakeContact(21, TouchReportUp));
    contacts.push_back(MakeContact(22, TouchReportUp));
    contacts.push_back(MakeContact(23, TouchReportUp));

    const auto packets = VhfTouchPacket::Build(contacts, false);

    Require(ContactCount(packets) == 10,
            "capacity pressure should still cap output at ten contacts");
    Require(ContactId(packets, 0) == 21 && ContactState(packets, 0) == 0x02,
            "first Up should be packed first");
    Require(ContactId(packets, 1) == 22 && ContactState(packets, 1) == 0x02,
            "second Up should be packed second");
    Require(ContactId(packets, 2) == 23 && ContactState(packets, 2) == 0x02,
            "third Up should be packed third");
    Require(ContactId(packets, 3) == 1 && ContactId(packets, 9) == 7,
            "remaining capacity should be filled by the earliest move contacts");
}

void TestMoveOnlyOrderMatchesInputOrder() {
    std::vector<TouchContact> contacts;
    for (int id = 1; id <= 10; ++id) {
        contacts.push_back(MakeContact(id, TouchReportMove));
    }

    const auto packets = VhfTouchPacket::Build(contacts, false);

    Require(ContactCount(packets) == 10,
            "move-only report should preserve existing ten-contact capacity");
    Require(ContactState(packets, 0) == 0x03 && ContactId(packets, 0) == 1,
            "move-only first contact should remain first");
    Require(ContactState(packets, 9) == 0x03 && ContactId(packets, 9) == 10,
            "move-only tenth contact should remain tenth");
}

void TestHiddenAndInvalidContactsAreSkipped() {
    std::vector<TouchContact> contacts;
    contacts.push_back(MakeContact(0, TouchReportMove));
    contacts.push_back(MakeContact(-1, TouchReportMove));
    contacts.push_back(MakeContact(1, TouchReportMove));
    contacts.push_back(MakeContact(2, TouchReportUp, false));
    contacts.push_back(MakeContact(3, TouchReportMove, false));
    contacts.push_back(MakeContact(4, TouchReportUp));

    const auto packets = VhfTouchPacket::Build(contacts, false);

    Require(ContactCount(packets) == 2,
            "hidden and non-positive id contacts should not be packed");
    Require(ContactState(packets, 0) == 0x02 && ContactId(packets, 0) == 4,
            "visible Up should be packed before visible move");
    Require(ContactState(packets, 1) == 0x03 && ContactId(packets, 1) == 1,
            "visible move should fill remaining capacity after Up");
}

void TestCoordinateMappingAndIdClamp() {
    TouchContact contact = MakeContact(300, TouchReportMove);
    contact.x = 15.0f;
    contact.y = 10.0f;

    auto packets = VhfTouchPacket::Build(std::vector<TouchContact>{contact}, false);
    Require(ContactId(packets, 0) == 255, "touch id should clamp to 255");
    Require(ContactY(packets, 0) == 4000,
            "non-transposed touch should map Y directly to logical Y");
    Require(ContactX(packets, 0) == 19200,
            "non-transposed touch should invert X into logical X");

    packets = VhfTouchPacket::Build(std::vector<TouchContact>{contact}, true);
    Require(ContactY(packets, 0) == 12000,
            "transposed touch should invert Y into logical Y");
    Require(ContactX(packets, 0) == 6400,
            "transposed touch should map X directly to logical X");
}

void TestPacketValidityAndCountsAtBoundaries() {
    const auto emptyPackets = VhfTouchPacket::Build(std::vector<TouchContact>{}, false);
    Require(!emptyPackets[0].valid && !emptyPackets[1].valid,
            "empty touch report should mark both packets invalid");
    Require(emptyPackets[0].bytes[0] == 0x01 && emptyPackets[1].bytes[0] == 0x01,
            "empty touch report should still preserve touch report ids");
    Require(ContactCount(emptyPackets) == 0,
            "empty touch report should set contact count to zero");

    std::vector<TouchContact> fiveContacts;
    for (int id = 1; id <= 5; ++id) {
        fiveContacts.push_back(MakeContact(id, TouchReportMove));
    }
    const auto fivePackets = VhfTouchPacket::Build(fiveContacts, false);
    Require(fivePackets[0].valid && !fivePackets[1].valid,
            "five contacts should use only the first touch packet");
    Require(ContactCount(fivePackets) == 5,
            "five-contact report should store exact contact count");

    fiveContacts.push_back(MakeContact(6, TouchReportMove));
    const auto sixPackets = VhfTouchPacket::Build(fiveContacts, false);
    Require(sixPackets[0].valid && sixPackets[1].valid,
            "six contacts should mark both touch packets valid");
    Require(ContactCount(sixPackets) == 6,
            "six-contact report should store exact contact count in packet0");
}

} // namespace

int main() {
    try {
        TestLateUpPreemptsTenthMove();
        TestNineMovesAndOneUpAreAllPacked();
        TestMultipleUpsPreemptMovesUnderCapacityPressure();
        TestMoveOnlyOrderMatchesInputOrder();
        TestHiddenAndInvalidContactsAreSkipped();
        TestCoordinateMappingAndIdClamp();
        TestPacketValidityAndCountsAtBoundaries();
        std::cout << "[TEST] Device VHF reporter touch packet tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
