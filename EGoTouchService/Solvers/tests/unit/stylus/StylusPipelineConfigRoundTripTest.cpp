#include "StylusSolver/StylusPipeline.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void LoadFromSavedText(Solvers::StylusPipeline& pipeline, const std::string& saved) {
    std::istringstream in(saved);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        pipeline.LoadConfig(line.substr(0, eq), line.substr(eq + 1));
    }
}

void RequireSavedContains(const std::string& saved, const char* entry, const char* message) {
    Require(saved.find(entry) != std::string::npos, message);
}

const Solvers::ConfigParam* FindSchemaEntry(const std::vector<Solvers::ConfigParam>& schema,
                                            const char* key) {
    for (const auto& param : schema) {
        if (param.key == key) {
            return &param;
        }
    }
    return nullptr;
}

void RequireSchemaEntry(const std::vector<Solvers::ConfigParam>& schema,
                        const char* key,
                        Solvers::ConfigParam::Type type,
                        const char* module) {
    const auto* entry = FindSchemaEntry(schema, key);
    Require(entry != nullptr, "schema should contain expected entry");
    Require(entry->type == type, "schema entry type should match expected type");
    Require(entry->moduleTag == module, "schema entry module should match expected module");
}

void TestStylusPipelineRoundTripIncludesCoordinateAndPostPressure() {
    Solvers::StylusPipeline pipeline;
    pipeline.m_hpp3.m_tiltProcess.m_enabled = false;
    pipeline.m_hpp3.m_coordinateSolver.m_signalFloor = 77;
    pipeline.m_hpp3.m_postPressure.m_enabled = false;
    pipeline.m_hpp3.m_postPressure.m_fakePressureDecreaseEnabled = false;
    pipeline.m_hpp3.m_postPressure.m_btFreqShiftDebounceFrames = 7;
    pipeline.m_hpp3.m_postPressure.m_pressureEdgeEnterThreshold = 910;
    pipeline.m_hpp3.m_postPressure.m_pressureEdgeExitThreshold = 456;
    pipeline.m_hpp3.m_pressureSolver.m_btPressSignalSuppressEnterThreshold = 321;
    pipeline.m_hpp3.m_pressureSolver.m_btPressSignalSuppressExitThreshold = 123;

    std::ostringstream out;
    pipeline.SaveConfig(out);
    const std::string saved = out.str();

    RequireSavedContains(saved,
                         "sp.tiltProcessEnabled=0",
                         "saved config should include tilt process enabled flag");
    RequireSavedContains(saved,
                         "sp.signalFloor=77",
                         "saved config should include coordinate signal floor");
    RequireSavedContains(saved,
                         "sp.postPressureEnabled=0",
                         "saved config should contain post-pressure enable flag");
    RequireSavedContains(saved,
                         "sp.fakePressureDecreaseEnabled=0",
                         "saved config should contain fake pressure flag");
    RequireSavedContains(saved,
                         "sp.btFreqShiftDebounceFrames=7",
                         "saved config should contain BT freq debounce frames");
    RequireSavedContains(saved,
                         "sp.pressureEdgeEnterThreshold=910",
                         "saved config should preserve enter threshold key");
    RequireSavedContains(saved,
                         "sp.pressureEdgeExitThreshold=456",
                         "saved config should preserve exit threshold key");
    RequireSavedContains(saved,
                         "sp.btPressSignalSuppressEnterThreshold=321",
                         "saved config should contain BT pressure suppress enter threshold");
    RequireSavedContains(saved,
                         "sp.btPressSignalSuppressExitThreshold=123",
                         "saved config should contain BT pressure suppress exit threshold");

    Solvers::StylusPipeline restored;
    LoadFromSavedText(restored, saved);

    Require(!restored.m_hpp3.m_tiltProcess.m_enabled,
            "loaded config should restore tilt process enabled flag");
    Require(restored.m_hpp3.m_coordinateSolver.m_signalFloor == 77,
            "loaded config should restore coordinate signal floor");
    Require(!restored.m_hpp3.m_postPressure.m_enabled,
            "loaded config should restore post-pressure enable flag");
    Require(!restored.m_hpp3.m_postPressure.m_fakePressureDecreaseEnabled,
            "loaded config should restore fake pressure flag");
    Require(restored.m_hpp3.m_postPressure.m_btFreqShiftDebounceFrames == 7,
            "loaded config should restore BT freq debounce frames");
    Require(restored.m_hpp3.m_postPressure.m_pressureEdgeEnterThreshold == 910,
            "loaded config should restore enter threshold");
    Require(restored.m_hpp3.m_postPressure.m_pressureEdgeExitThreshold == 456,
            "loaded config should restore exit threshold");
    Require(restored.m_hpp3.m_pressureSolver.m_btPressSignalSuppressEnterThreshold == 321,
            "loaded config should restore BT pressure suppress enter threshold");
    Require(restored.m_hpp3.m_pressureSolver.m_btPressSignalSuppressExitThreshold == 123,
            "loaded config should restore BT pressure suppress exit threshold");
}

void TestHpp2ConfigRoundTripAndSchema() {
    Solvers::StylusPipeline pipeline;
    pipeline.m_hpp2.m_enabled = true;
    // Use near-default non-default HPP2 values so the test exercises round-trip
    // behavior without implying unrelated production thresholds.
    pipeline.m_hpp2.m_sensorTxCount = 58;
    pipeline.m_hpp2.m_sensorRxCount = 39;
    pipeline.m_hpp2.m_cmfWindowRadius = 5;
    pipeline.m_hpp2.m_rawAbnormalLineSumThreshold = 32000;
    pipeline.m_hpp2.m_rawAbnormalEnergyRatioThreshold = 321;
    pipeline.m_hpp2.m_cmnAbnormalSumThreshold = 9500;
    pipeline.m_hpp2.m_cmnAbnormalMinThreshold = 2345;
    pipeline.m_hpp2.m_chargerNoiseSumThreshold = 450;
    pipeline.m_hpp2.m_peakSignalFloor = 456;
    pipeline.m_hpp2.m_pressureDeltaNormal = 777;
    pipeline.m_hpp2.m_pressureDeltaTight = 88;
    pipeline.m_hpp2.m_useTightPressureDelta = true;

    const auto schema = pipeline.GetConfigSchema();
    RequireSchemaEntry(schema, "hpp2.enabled", Solvers::ConfigParam::Bool, "HPP2");
    RequireSchemaEntry(schema, "hpp2.rawAbnormalLineSumThreshold", Solvers::ConfigParam::UInt32, "HPP2");
    RequireSchemaEntry(schema, "hpp2.cmnAbnormalSumThreshold", Solvers::ConfigParam::UInt32, "HPP2");
    RequireSchemaEntry(schema, "hpp2.chargerNoiseSumThreshold", Solvers::ConfigParam::UInt32, "HPP2");
    RequireSchemaEntry(schema, "hpp2.pressureDeltaNormal", Solvers::ConfigParam::UInt16, "HPP2");

    std::ostringstream out;
    pipeline.SaveConfig(out);
    const std::string saved = out.str();

    RequireSavedContains(saved,
                         "hpp2.sensorTxCount=58",
                         "saved config should include HPP2 TX count");
    RequireSavedContains(saved,
                         "hpp2.rawAbnormalLineSumThreshold=32000",
                         "saved config should include HPP2 uint32 raw threshold");
    RequireSavedContains(saved,
                         "hpp2.cmnAbnormalSumThreshold=9500",
                         "saved config should include HPP2 uint32 CMN threshold");
    RequireSavedContains(saved,
                         "hpp2.chargerNoiseSumThreshold=450",
                         "saved config should include HPP2 uint32 charger threshold");
    RequireSavedContains(saved,
                         "hpp2.useTightPressureDelta=1",
                         "saved config should include HPP2 tight pressure flag");

    Solvers::StylusPipeline restored;
    LoadFromSavedText(restored, saved);

    Require(restored.m_hpp2.m_sensorTxCount == 58,
            "loaded config should restore HPP2 TX count");
    Require(restored.m_hpp2.m_sensorRxCount == 39,
            "loaded config should restore HPP2 RX count");
    Require(restored.m_hpp2.m_cmfWindowRadius == 5,
            "loaded config should restore HPP2 CMF radius");
    Require(restored.m_hpp2.m_rawAbnormalLineSumThreshold == 32000,
            "loaded config should restore HPP2 raw threshold");
    Require(restored.m_hpp2.m_rawAbnormalEnergyRatioThreshold == 321,
            "loaded config should restore HPP2 raw energy ratio");
    Require(restored.m_hpp2.m_cmnAbnormalSumThreshold == 9500,
            "loaded config should restore HPP2 CMN sum threshold");
    Require(restored.m_hpp2.m_cmnAbnormalMinThreshold == 2345,
            "loaded config should restore HPP2 CMN min threshold");
    Require(restored.m_hpp2.m_chargerNoiseSumThreshold == 450,
            "loaded config should restore HPP2 charger noise sum threshold");
    Require(restored.m_hpp2.m_peakSignalFloor == 456,
            "loaded config should restore HPP2 peak signal floor");
    Require(restored.m_hpp2.m_pressureDeltaNormal == 777,
            "loaded config should restore HPP2 normal pressure delta");
    Require(restored.m_hpp2.m_pressureDeltaTight == 88,
            "loaded config should restore HPP2 tight pressure delta");
    Require(restored.m_hpp2.m_useTightPressureDelta,
            "loaded config should restore HPP2 tight pressure flag");
}

} // namespace

int main() {
    try {
        TestStylusPipelineRoundTripIncludesCoordinateAndPostPressure();
        TestHpp2ConfigRoundTripAndSchema();
        std::cout << "[TEST] Stylus pipeline config round-trip tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] " << ex.what() << "\n";
        return 1;
    }
}
