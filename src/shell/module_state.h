#pragma once

// Process-wide state shared by the COM objects and the DLL entry points.
// The shell host may unload the DLL only after every object and every
// in-flight decode thread has finished.

namespace nv::shell {

void AddLiveObject();
void ReleaseLiveObject();

void AddLockRef();
void ReleaseLockRef();

void BeginDecode();
void EndDecode();

bool CanUnloadModule();

// RAII helper for decode threads. The shell DLL keeps itself loaded while a
// detached decode thread is still running after a timeout.
struct DecodeActivity {
  DecodeActivity() { BeginDecode(); }
  ~DecodeActivity() { EndDecode(); }
  DecodeActivity(const DecodeActivity&) = delete;
  DecodeActivity& operator=(const DecodeActivity&) = delete;
};

}  // namespace nv::shell
