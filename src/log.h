#pragma once

#if !defined(TEST_ENVIRONMENT)
#include <spdlog/sinks/basic_file_sink.h>
#include <filesystem>
#include <format>

// How many logs to keep in total: the current run plus KEEP_LOGS-1 previous ones.
inline constexpr int KEEP_LOGS = 4;

// Shift the previous run's logs down a slot so a crash report isn't lost the next
// time the game starts. Name.log -> Name.1.log -> Name.2.log -> ... -> deleted.
inline void RotateLogs(const std::filesystem::path& logFile) {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(logFile, ec)) return;

    const auto dir = logFile.parent_path();
    const auto stem = logFile.stem().string();
    const auto slot = [&](int n) { return dir / std::format("{}.{}.log", stem, n); };

    fs::remove(slot(KEEP_LOGS - 1), ec);
    for (int n = KEEP_LOGS - 2; n >= 1; --n) {
        fs::rename(slot(n), slot(n + 1), ec);  // no-op when that slot is empty
    }
    fs::rename(logFile, slot(1), ec);
}

inline void SetupLog() {
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    auto logFilePath = *logsFolder / std::format("{}.log", pluginName);
    RotateLogs(logFilePath);
    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
}
#else
// Test environment - logging is stubbed in TestStubs.h
inline void SetupLog() {}
#endif
