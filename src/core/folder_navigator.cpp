#include "core/folder_navigator.h"

#include "core/format_router.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <string>

namespace nv {
namespace {

std::wstring LowerPath(const std::filesystem::path& path) {
  std::wstring text = path.lexically_normal().wstring();
  std::ranges::transform(text, text.begin(),
                         [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return text;
}

// The one folder order (v3.0, 規劃/26 §2.5): Explorer's natural name order.
// `StrCmpLogicalW` numbers digit runs ("img2" < "img10") and compares
// case-insensitively; when it reports equality (e.g. "A.bmp" vs "a.bmp") an
// ordinal comparison decides, so the result never depends on the scan order.
int CompareNatural(const std::filesystem::path& a,
                   const std::filesystem::path& b) {
  const std::wstring an = a.filename().native();
  const std::wstring bn = b.filename().native();
  const int natural = StrCmpLogicalW(an.c_str(), bn.c_str());
  if (natural != 0) return natural < 0 ? -1 : 1;
  if (an != bn) return an < bn ? -1 : 1;
  const std::wstring ap = a.native();
  const std::wstring bp = b.native();
  if (ap != bp) return ap < bp ? -1 : 1;
  return 0;
}

// Scans `folder` for supported images, sorted. The caller decides whether to
// treat an empty result as an error (file open) or as the empty-folder state
// (folder drop) - the scan itself never fails silently into a wrong folder.
void ScanFolder(const std::filesystem::path& folder, FolderList* list) {
  std::error_code ec;
  std::filesystem::directory_iterator it(
      folder, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) return;

  for (const auto& entry : it) {
    if (!entry.is_regular_file(ec)) continue;
    std::wstring extension = entry.path().extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t c) {
      return static_cast<wchar_t>(std::towlower(c));
    });
    if (!FormatRouter::IsSupportedExtension(extension)) continue;
    list->files.push_back(entry.path().lexically_normal());
  }

  std::ranges::sort(list->files, [](const auto& a, const auto& b) {
    return CompareNatural(a, b) < 0;
  });
}

}  // namespace

FolderList BuildFolderList(const std::filesystem::path& current) {
  FolderList list;
  list.folder = current.parent_path();
  if (list.folder.empty()) {
    std::error_code ec;
    list.folder = std::filesystem::current_path(ec);
  }
  ScanFolder(list.folder, &list);

  const std::wstring current_key = LowerPath(current);
  for (size_t i = 0; i < list.files.size(); ++i) {
    if (LowerPath(list.files[i]) == current_key) {
      list.index = i;
      break;
    }
  }
  return list;
}

FolderList BuildFolderListFromFolder(const std::filesystem::path& folder) {
  FolderList list;
  list.folder = folder.lexically_normal();
  if (list.folder.empty()) {
    std::error_code ec;
    list.folder = std::filesystem::current_path(ec);
  }
  ScanFolder(list.folder, &list);
  list.index = 0;
  return list;
}

bool StepFolder(FolderList& list, int delta) {
  if (list.files.empty()) return false;
  const int size = static_cast<int>(list.files.size());
  int next = static_cast<int>(list.index) + delta;
  next %= size;
  if (next < 0) next += size;
  list.index = static_cast<size_t>(next);
  return true;
}

const wchar_t* WrapStrategyName(WrapStrategy strategy) {
  switch (strategy) {
    case WrapStrategy::kLoop:
      return L"loop";
    case WrapStrategy::kNext:
      return L"next";
    case WrapStrategy::kAsk:
      return L"ask";
  }
  return L"loop";
}

bool ParseWrapStrategy(std::wstring_view text, WrapStrategy* strategy) {
  if (strategy == nullptr) return false;
  if (text == L"loop") {
    *strategy = WrapStrategy::kLoop;
    return true;
  }
  if (text == L"next") {
    *strategy = WrapStrategy::kNext;
    return true;
  }
  if (text == L"ask") {
    *strategy = WrapStrategy::kAsk;
    return true;
  }
  return false;
}

StepAction PlanFolderStep(size_t index, size_t count, int direction,
                          WrapStrategy strategy, bool has_adjacent) {
  if (count == 0) return StepAction::kMove;
  const long long next =
      static_cast<long long>(index) + (direction < 0 ? -1 : 1);
  if (next >= 0 && next < static_cast<long long>(count)) {
    return StepAction::kMove;
  }
  switch (strategy) {
    case WrapStrategy::kLoop:
      return StepAction::kLoop;
    case WrapStrategy::kNext:
      // The last image folder still wraps inside itself instead of getting
      // stuck, so "next" is always a pure superset of "loop" (22/10 §4.2).
      return has_adjacent ? StepAction::kAdjacent : StepAction::kLoop;
    case WrapStrategy::kAsk:
      return has_adjacent ? StepAction::kAskAdjacent : StepAction::kAskLoop;
  }
  return StepAction::kLoop;
}

std::vector<std::filesystem::path> ListSiblingFolders(
    const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> siblings;
  std::error_code ec;
  std::filesystem::directory_iterator it(
      folder, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) return siblings;

  for (const auto& entry : it) {
    std::error_code entry_ec;
    if (!entry.is_directory(entry_ec)) continue;
    siblings.push_back(entry.path().lexically_normal());
  }
  std::ranges::sort(siblings, [](const auto& a, const auto& b) {
    return CompareNatural(a, b) < 0;
  });
  return siblings;
}

bool FolderHasSupportedImages(const std::filesystem::path& folder) {
  std::error_code ec;
  std::filesystem::directory_iterator it(
      folder, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) return false;
  for (const auto& entry : it) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec)) continue;
    std::wstring extension = entry.path().extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t c) {
      return static_cast<wchar_t>(std::towlower(c));
    });
    if (FormatRouter::IsSupportedExtension(extension)) return true;
  }
  return false;
}

std::optional<std::filesystem::path> FindAdjacentImageFolder(
    const std::vector<std::filesystem::path>& siblings,
    const std::filesystem::path& folder, int direction, size_t max_probe,
    size_t* probed) {
  if (probed != nullptr) *probed = 0;
  if (siblings.empty() || direction == 0) return std::nullopt;

  const std::wstring key = LowerPath(folder);
  size_t start = siblings.size();
  for (size_t i = 0; i < siblings.size(); ++i) {
    if (LowerPath(siblings[i]) == key) {
      start = i;
      break;
    }
  }
  if (start == siblings.size()) return std::nullopt;

  size_t inspected = 0;
  const int step = direction < 0 ? -1 : 1;
  for (long long i = static_cast<long long>(start) + step;
       i >= 0 && i < static_cast<long long>(siblings.size());
       i += step) {
    if (inspected >= max_probe) break;
    ++inspected;
    if (FolderHasSupportedImages(siblings[static_cast<size_t>(i)])) {
      if (probed != nullptr) *probed = inspected;
      return siblings[static_cast<size_t>(i)];
    }
  }
  if (probed != nullptr) *probed = inspected;
  return std::nullopt;
}

std::optional<std::filesystem::path> FindAdjacentImageFolder(
    const std::filesystem::path& folder, int direction, size_t max_probe,
    size_t* probed) {
  // Siblings live in the folder's parent, not in the folder itself.
  return FindAdjacentImageFolder(ListSiblingFolders(folder.parent_path()),
                                 folder, direction, max_probe, probed);
}

bool RescanFolder(FolderList& list, const std::filesystem::path& keep) {
  const std::filesystem::path folder = list.folder;
  const size_t previous_index = list.index;
  FolderList fresh = BuildFolderListFromFolder(folder);
  if (!fresh.empty()) {
    fresh.index = std::min(previous_index, fresh.size() - 1);
  }
  const std::wstring key = LowerPath(keep);
  bool found = false;
  for (size_t i = 0; i < fresh.files.size(); ++i) {
    if (LowerPath(fresh.files[i]) == key) {
      fresh.index = i;
      found = true;
      break;
    }
  }
  list = std::move(fresh);
  return found;
}

}  // namespace nv
