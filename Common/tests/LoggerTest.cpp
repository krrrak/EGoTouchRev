#include "Logger.h"

#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class CapturingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> Lines() const {
        std::lock_guard<std::mutex> lock(linesMutex_);
        return lines_;
    }

    bool Contains(std::string_view text) const {
        auto lines = Lines();
        return std::any_of(lines.begin(), lines.end(), [text](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        std::string line = fmt::to_string(formatted);
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        std::lock_guard<std::mutex> lock(linesMutex_);
        lines_.push_back(std::move(line));
    }

    void flush_() override {}

private:
    mutable std::mutex linesMutex_;
    std::vector<std::string> lines_;
};

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void ShutdownLogger() {
    Common::Logger::Shutdown();
    spdlog::drop_all();
}

std::filesystem::path TestPath(const std::string& name) {
    return std::filesystem::current_path() / name;
}

bool WaitForContains(const std::shared_ptr<CapturingSink>& sink, std::string_view text) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (sink->Contains(text)) {
            return true;
        }
        if (Common::Logger::Get()) {
            Common::Logger::Get()->flush();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return sink->Contains(text);
}

void TestInvalidDirectoryDoesNotInitialize() {
    ShutdownLogger();
    const auto invalidDir = TestPath("CommonLoggerTest_invalid_dir_marker");
    std::filesystem::remove_all(invalidDir);

    {
        std::ofstream out(invalidDir);
        out << "This is a file, not a directory";
    }

    std::stringstream buffer;
    std::streambuf* oldCerr = std::cerr.rdbuf(buffer.rdbuf());
    Common::Logger::Init("CommonLoggerTestInvalid", invalidDir);
    std::cerr.rdbuf(oldCerr);

    const std::string errOutput = buffer.str();
    std::filesystem::remove(invalidDir);

    Require(errOutput.find("Failed to create log directory") != std::string::npos,
            "Logger::Init should report create_directories failure to stderr");
    Require(Common::Logger::Get() == nullptr,
            "Logger::Init should leave Logger::Get null after directory creation failure");
}

void TestInitializeRepeatAndShutdown() {
    ShutdownLogger();
    const auto logDir = TestPath("CommonLoggerTest_repeat_logs");
    std::filesystem::remove_all(logDir);

    auto firstSink = std::make_shared<CapturingSink>();
    auto secondSink = std::make_shared<CapturingSink>();

    Common::Logger::Init("CommonLoggerTestRepeatA", logDir, firstSink);
    auto firstLogger = Common::Logger::Get();
    Require(firstLogger != nullptr, "Logger::Init should create a logger for a valid directory");

    Common::Logger::Init("CommonLoggerTestRepeatB", logDir, secondSink);
    Require(Common::Logger::Get() == firstLogger,
            "Second Logger::Init call should not replace an existing logger");

    LOG_WARN("Common", "LoggerTest", "RepeatInit", "repeat-init marker {}", 17);
    Require(WaitForContains(firstSink, "repeat-init marker 17"),
            "First extra sink should receive macro output after repeat init");
    Require(!secondSink->Contains("repeat-init marker 17"),
            "Second extra sink should not be attached when Logger::Init is ignored");

    ShutdownLogger();
    firstLogger.reset();
    Require(Common::Logger::Get() == nullptr, "Logger::Shutdown should clear Logger::Get");
    std::filesystem::remove_all(logDir);
}

void TestExtraSinkReceivesFormattedMacroOutput() {
    ShutdownLogger();
    const auto logDir = TestPath("CommonLoggerTest_extra_sink_logs");
    std::filesystem::remove_all(logDir);

    auto sink = std::make_shared<CapturingSink>();
    Common::Logger::Init("CommonLoggerTestExtraSink", logDir, sink);
    Require(Common::Logger::Get() != nullptr, "Logger::Init should succeed with an extra sink");

    LOG_INFO("LayerA", "MethodB", "StateC", "value {}", 42);
    Require(WaitForContains(sink, "[LayerA] [MethodB] [StateC] value 42"),
            "LOG_INFO should emit formatted layer/method/state message to extra sink");

    ShutdownLogger();
    std::filesystem::remove_all(logDir);
}

} // namespace

int main() {
    try {
        TestInvalidDirectoryDoesNotInitialize();
        TestInitializeRepeatAndShutdown();
        TestExtraSinkReceivesFormattedMacroOutput();
        ShutdownLogger();
        std::cout << "[TEST] CommonLoggerTest passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[TEST] CommonLoggerTest failed: " << ex.what() << '\n';
        ShutdownLogger();
        return 1;
    }
}
