#pragma once

#include <cstddef>

namespace nv::platform {

// Debug-CRT heap accounting. This is the MSVC counterpart to LeakSanitizer:
// AddressSanitizer on Windows reports memory errors but not leaks, so the leak
// harness compares _CrtMemState checkpoints before and after each workload
// round. All functions are no-ops in Release and in ASan builds.
struct HeapUsage {
  size_t bytes = 0;
  size_t blocks = 0;
};

bool LeakTrackingEnabled();

// Enables the Debug heap and routes the CRT leak report to stderr.
void InitializeLeakTracking();

// Live Debug-heap bytes/blocks at this point.
HeapUsage SnapshotHeap();

// Prints "<label>: bytes=+N blocks=+N" and returns the growth. A positive
// byte or block delta across a repeated workload round is a leak candidate;
// callers compare several rounds because DLL/CRT lazy initialization can
// allocate once.
HeapUsage ReportHeapGrowth(const char* label, const HeapUsage& before,
                           const HeapUsage& after);

// Reports any blocks still allocated since process start and returns true when
// the CRT found at least one. Noisy for a GUI process (COM/D3D keep process
// globals), so the harness uses it for targeted, short-lived checks only.
bool DumpLeaks(const char* label);

}  // namespace nv::platform
