#include "platform/leak_check.h"

#if defined(_DEBUG) && !defined(NV_ASAN)
#include <crtdbg.h>

#include <cstdio>
#endif

namespace nv::platform {

#if defined(_DEBUG) && !defined(NV_ASAN)

namespace {

size_t TotalBytes(const _CrtMemState& state) {
  size_t total = 0;
  for (int i = 0; i < _MAX_BLOCKS; ++i) total += state.lSizes[i];
  return total;
}

size_t TotalBlocks(const _CrtMemState& state) {
  size_t total = 0;
  for (int i = 0; i < _MAX_BLOCKS; ++i) total += state.lCounts[i];
  return total;
}

}  // namespace

bool LeakTrackingEnabled() { return true; }

void InitializeLeakTracking() {
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
  // Keep the end-of-process report on stderr so the harness can capture it.
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
}

HeapUsage SnapshotHeap() {
  _CrtMemState state{};
  _CrtMemCheckpoint(&state);
  return HeapUsage{TotalBytes(state), TotalBlocks(state)};
}

HeapUsage ReportHeapGrowth(const char* label, const HeapUsage& before,
                           const HeapUsage& after) {
  const long long bytes =
      static_cast<long long>(after.bytes) - static_cast<long long>(before.bytes);
  const long long blocks =
      static_cast<long long>(after.blocks) - static_cast<long long>(before.blocks);
  std::printf("heap %-28s bytes=%+lld blocks=%+lld\n", label, bytes, blocks);
  return HeapUsage{static_cast<size_t>(bytes < 0 ? 0 : bytes),
                   static_cast<size_t>(blocks < 0 ? 0 : blocks)};
}

bool DumpLeaks(const char* label) {
  std::fprintf(stderr, "--- leak report: %s ---\n", label);
  std::fflush(stderr);
  return _CrtDumpMemoryLeaks() != 0;
}

#else

bool LeakTrackingEnabled() { return false; }
void InitializeLeakTracking() {}
HeapUsage SnapshotHeap() { return {}; }
HeapUsage ReportHeapGrowth(const char*, const HeapUsage&, const HeapUsage&) {
  return {};
}
bool DumpLeaks(const char*) { return false; }

#endif

}  // namespace nv::platform
