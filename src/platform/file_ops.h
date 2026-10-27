#pragma once

#include <filesystem>
#include <string>

namespace nv::platform {

bool DeleteToRecycleBin(const std::filesystem::path& path, std::wstring* error);
bool RenamePath(const std::filesystem::path& from, const std::filesystem::path& to,
                std::wstring* error);
bool SetDesktopWallpaper(const std::filesystem::path& path, std::wstring* error);
bool RevealInExplorer(const std::filesystem::path& path, std::wstring* error);

}  // namespace nv::platform
