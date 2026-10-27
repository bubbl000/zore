#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace nv {

// v3.0 (規劃/26 §2.5): the folder has exactly one order — the one Explorer
// shows. `StrCmpLogicalW` compares digit runs by value ("img2" before "img10")
// and is case-insensitive; names that still compare equal get a deterministic
// ordinal tie-break so the list never depends on the scan order.
//
// There used to be a `SortMode` (name / date / size) selected from the 更多▾
// panel and the settings page; both entry points are gone, and so is the enum.
struct FolderList {
  std::filesystem::path folder;
  std::vector<std::filesystem::path> files;
  size_t index = 0;

  bool empty() const { return files.empty(); }
  const std::filesystem::path& current() const { return files[index]; }
  size_t size() const { return files.size(); }
};

// Lists supported images in the current file's folder, in Explorer's natural
// name order. The current file is located by path (規劃/22/02 §7).
FolderList BuildFolderList(const std::filesystem::path& current);

// Lists the supported images of a folder itself (dropped/opened folder). The
// index is 0; callers open `files[0]` or show the empty-folder state
// (規劃/22/03 §4.3).
FolderList BuildFolderListFromFolder(const std::filesystem::path& folder);

// Moves by delta, wrapping at the ends. Returns false when the list is empty.
bool StepFolder(FolderList& list, int delta);

// --- v2.0 S2 / 22/10: folder wrap and folder peeking ------------------------

// "At folder edge" policy, parsed from platform::Settings.folder_wrap.
enum class WrapStrategy {
  kLoop = 0,
  kNext,
  kAsk,
};

const wchar_t* WrapStrategyName(WrapStrategy strategy);
bool ParseWrapStrategy(std::wstring_view text, WrapStrategy* strategy);

// Pure decision for one step, so the three policies are unit-testable without
// a window. `has_adjacent` says whether the neighbouring folder (in the step's
// direction) exists *and* contains at least one supported image.
enum class StepAction {
  kMove = 0,      // the next index is inside the current list
  kLoop,          // wrap inside the current folder (first/last)
  kAdjacent,      // enter the adjacent folder at its first/last image
  kAskAdjacent,   // ask, then enter the adjacent folder
  kAskLoop,       // ask, then wrap inside the current folder
};

StepAction PlanFolderStep(size_t index, size_t count, int direction,
                          WrapStrategy strategy, bool has_adjacent);

// Enumerates the immediate subdirectories of `folder`, sorted with the same
// natural order as the image list (Explorer's). The folder itself is not
// included; unreadable entries are skipped.
std::vector<std::filesystem::path> ListSiblingFolders(
    const std::filesystem::path& folder);

// True when the folder holds at least one supported image. Used to skip empty
// or unsupported sibling folders without scanning the whole tree (22/10 坑 12).
bool FolderHasSupportedImages(const std::filesystem::path& folder);

// Finds the nearest sibling folder in `direction` (+1 = after, -1 = before)
// that contains at least one supported image. At most `max_probe` directories
// are inspected; `probed` (optional) receives the number actually inspected.
std::optional<std::filesystem::path> FindAdjacentImageFolder(
    const std::vector<std::filesystem::path>& siblings,
    const std::filesystem::path& folder, int direction, size_t max_probe = 200,
    size_t* probed = nullptr);
std::optional<std::filesystem::path> FindAdjacentImageFolder(
    const std::filesystem::path& folder, int direction, size_t max_probe = 200,
    size_t* probed = nullptr);

// Rebuilds `list` from its own folder and re-locates `keep` by path (watch
// rescan, 22/10 §3.1). Returns false when `keep` is no longer listed; the
// index is then clamped to a valid entry so callers can show the next image.
bool RescanFolder(FolderList& list, const std::filesystem::path& keep);

}  // namespace nv
