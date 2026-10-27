#pragma once

#include <string_view>

namespace nv::platform {

enum class LogLevel {
  kInfo,
  kWarn,
  kError,
};

void LogInit();
void LogShutdown();
void Log(LogLevel level, std::string_view message);

inline void LogInfo(std::string_view message) { Log(LogLevel::kInfo, message); }
inline void LogWarn(std::string_view message) { Log(LogLevel::kWarn, message); }
inline void LogError(std::string_view message) { Log(LogLevel::kError, message); }

}  // namespace nv::platform
