#pragma once

namespace nv::tests {

// v2.0 S2 / 22/10: folder-wrap decisions, sibling-folder peeking, rescan and
// the watch state machine. Uses temporary directories; no window or timer.
int RunFolderWatchTests();

}  // namespace nv::tests
