#include "StylusSolver/StylusPipeline.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

using Solvers::HeatmapFrame;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

HeatmapFrame MakeHpp2Frame(int dim1Peak,
                           int dim2Peak,
                           uint16_t dim1PeakValue,
                           uint16_t dim2PeakValue,
                           uint16_t pressure,
                           uint32_t buttonBits = 0) {
    HeatmapFrame frame{};
    auto& input = frame.stylus.input;
    input.auxStatusFlags = 0x1; // HPP2 protocol, not HPP3
    input.mainFreq = 0x00b0;
    input.auxFreq = 0x00fc;
    input.framePressure = pressure;
    input.buttonBits = buttonBits;
    input.hpp2LineValid = true;
    input.hpp2LineData.fill(10);
    input.hpp2LineData[static_cast<std::size_t>(dim1Peak)] = dim1PeakValue;
    input.hpp2LineData[static_cast<std::size_t>(60 + dim2Peak)] = dim2PeakValue;
    return frame;
}

void TestHpp2FrameProducesReport() {
    Solvers::StylusPipeline pipeline;
    HeatmapFrame frame = MakeHpp2Frame(12, 7, 2600, 2400, 512, 1);

    pipeline.Process(frame);

    Require(!frame.stylus.runtime.flow.terminal, "HPP2 frame should run non-terminal");
    Require(frame.stylus.output.valid, "HPP2 output should be valid");
    Require(frame.stylus.output.inRange, "HPP2 output should be in range");
    Require(frame.stylus.output.tipDown, "HPP2 pressure should produce tip-down");
    Require(frame.stylus.output.pressure == 512, "HPP2 pressure should publish frame pressure");
    Require(frame.stylus.runtime.hpp2.buttonPressed, "HPP2 button bit should be debounced as pressed");
    Require(frame.stylus.output.point.x > 0.0f && frame.stylus.output.point.y > 0.0f,
            "HPP2 coordinate should be populated");
}

void TestHpp2EdgePressureGuardSuppressesPressure() {
    Solvers::StylusPipeline pipeline;
    HeatmapFrame frame = MakeHpp2Frame(0, 7, 1000, 2400, 512);

    pipeline.Process(frame);

    Require(frame.stylus.output.valid, "edge-guarded HPP2 output should still have coordinate");
    Require(frame.stylus.output.inRange, "edge-guarded HPP2 output should remain in range");
    Require(!frame.stylus.output.tipDown, "low edge signal should clear tip-down");
    Require(frame.stylus.output.pressure == 0, "low edge signal should clear pressure");
}

} // namespace

int main() {
    try {
        TestHpp2FrameProducesReport();
        TestHpp2EdgePressureGuardSuppressesPressure();
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] StylusHpp2PipelineTest: " << ex.what() << "\n";
        return 1;
    }
    std::cout << "[PASS] StylusHpp2PipelineTest\n";
    return 0;
}
