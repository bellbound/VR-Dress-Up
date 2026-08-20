#pragma once

#if !defined(TEST_ENVIRONMENT)
#include <spdlog/sinks/basic_file_sink.h>
#include <filesystem>
#include <format>
#include <string_view>

// How many logs to keep in total: the current run plus KEEP_LOGS-1 previous ones.
inline constexpr int KEEP_LOGS = 4;

// What a log written without an INI - or with an unreadable one - comes out at. Info is
// the level a user-sent report is expected to arrive at: every decision the mod makes
// about an NPC is still in it, but the per-frame and per-item chatter is not.
inline constexpr spdlog::level::level_enum DEFAULT_LOG_LEVEL = spdlog::level::info;

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

// Levels as they are spelled in the INI. Anything else - including an empty value, which
// is what a missing key reads back as - falls through to the default rather than silently
// turning logging off.
inline spdlog::level::level_enum ParseLogLevel(std::string_view name) {
    if (name == "trace") return spdlog::level::trace;
    if (name == "debug") return spdlog::level::debug;
    if (name == "info") return spdlog::level::info;
    if (name == "warn" || name == "warning") return spdlog::level::warn;
    if (name == "error" || name == "err") return spdlog::level::err;
    if (name == "off" || name == "none") return spdlog::level::off;
    return DEFAULT_LOG_LEVEL;
}

// "warn" and "warning", "error" and "err", "off" and "none" are all accepted above, so a
// spelling that differs from spdlog's own name for the level is still a spelling we meant.
inline bool IsKnownLogLevel(std::string_view name) {
    return name == "trace" || name == "debug" || name == "info" || name == "warn" ||
           name == "warning" || name == "error" || name == "err" ||
           name == "off" || name == "none";
}

// Flushing on every line is what makes the log useful after a crash - the last thing the
// mod did is on disk, not in a buffer that went down with the process. That is only
// affordable because the levels below the active one cost nothing to skip.
inline void SetLogLevel(spdlog::level::level_enum level) {
    spdlog::set_level(level);
    spdlog::flush_on(level);
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
    // Settings::Load raises or lowers this as soon as it has read the INI; until then the
    // handful of startup lines are written at the default.
    SetLogLevel(DEFAULT_LOG_LEVEL);
}
#else
// Test environment - logging is stubbed in TestStubs.h
inline void SetupLog() {}
#endif
