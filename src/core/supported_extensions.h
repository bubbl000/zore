#pragma once

#include <string>
#include <vector>

namespace nv {

// Every file extension the application can detect and decode, lowercase and
// including the leading dot. This is the single source of truth for Shell
// integration (thumbnail handler, context-menu verbs, optional associations)
// and for the folder navigator's extension filter.
//
// Keep this aligned with FormatRouter's extension table: a name here that the
// router cannot open would make Explorer call a handler that always fails.
const std::vector<std::wstring>& SupportedExtensions();

// True when `extension` (already lowercase) belongs to SupportedExtensions().
bool IsSupportedImageExtension(const std::wstring& extension);

// Extensions offered for Explorer integration that targets image documents:
// the three context-menu verbs, "open with" registration and optional default
// associations. Icon formats (.ico/.cur) are excluded because taking over the
// icon association, or offering "set as wallpaper" for a file icon, is
// surprising. Thumbnails still cover SupportedExtensions().
const std::vector<std::wstring>& ImageDocumentExtensions();
bool IsImageDocumentExtension(const std::wstring& extension);

}  // namespace nv
