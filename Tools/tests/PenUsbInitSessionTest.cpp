#include "btmcu/PenUsbInitSession.h"
#include "btmcu/PenUsbTypes.h"

#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestFactoryInitAdvancesOnScreenStatusResponse() {
    Himax::Pen::PenUsbInitSession session;

    auto start = session.OnConnected();
    Require(start == Himax::Pen::PenUsbInitAction::SendInitialQueries,
            "connection should send 0x7101 and the first 0x7701 query");

    auto penConnStatus = session.OnEvent(Himax::Pen::PenUsbEventCode::PenConnStatus);
    Require(penConnStatus == Himax::Pen::PenUsbInitAction::None,
            "0x71 should not be required before the first 0x7701 query");

    auto afterFirstMcuStatus = session.OnEvent(Himax::Pen::PenUsbEventCode::PenScreenStatus);
    Require(afterFirstMcuStatus == Himax::Pen::PenUsbInitAction::SendSecondMcuStatusQuery,
            "0x77 response to first 0x7701 should request second 0x7701 query");

    auto afterParamRequest = session.OnEvent(Himax::Pen::PenUsbEventCode::PenRepParam);
    Require(afterParamRequest == Himax::Pen::PenUsbInitAction::SendInitProtocolParams,
            "0x7B after second 0x7701 should send 0x7D01 init params");

    auto duplicateParamRequest = session.OnEvent(Himax::Pen::PenUsbEventCode::PenRepParam);
    Require(duplicateParamRequest == Himax::Pen::PenUsbInitAction::None,
            "duplicate 0x7B should not resend 0x7D01");
}

} // namespace

int main() {
    try {
        TestFactoryInitAdvancesOnScreenStatusResponse();
        std::cout << "[TEST] Pen USB init session tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
