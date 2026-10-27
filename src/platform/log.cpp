#include "platform/log.h"

#include "platform/paths.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace nv::platform {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
bool g_ready = false;

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kInfo: return "INFO";
    case LogLevel::kWarn: return "WARN";
    case LogLevel::kError: return "ERROR";
  }
  return "INFO";
}

}  // namespace

void LogInit() {
  std::lock_guard lock(g_mutex);
  if (g_ready) return;

  const std::filesystem::path dir = DataDir() / L"logs";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path = dir / L"zoreview.log";

  // Keep one previous log; avoid an unbounded file on long-running installs.
  if (std::filesystem::exists(path, ec) &&
      std::filesystem::file_size(path, ec) > 8ull * 1024 * 1024) {
    std::filesystem::path old = path;
    old += L".1";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(path, old, ec);
  }

  g_file.open(path, std::ios::app | std::ios::binary);
  g_ready = g_file.is_open();

  if (g_ready) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    char stamp[64]{};
    tm local{};
    localtime_s(&local, &time);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    g_file << "[" << stamp << "][INFO] log started\n";
    g_file.flush();
  }
}

void LogShutdown() {
  std::lock_guard lock(g_mutex);
  if (!g_ready) return;
  g_file << "[----][INFO] log stopped\n";
  g_file.flush();
  g_file.close();
  g_ready = false;
}

void Log(LogLevel level, std::string_view message) {
  std::lock_guard lock(g_mutex);
  if (!g_ready) return;

  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  char stamp[64]{};
  tm local{};
  localtime_s(&local, &time);
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

  g_file << "[" << stamp << "][" << LevelName(level) << "] ";
  g_file.write(message.data(), static_cast<std::streamsize>(message.size()));
  g_file << "\n";
  g_file.flush();
}

}  // namespace nv::platform
