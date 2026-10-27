#include "shell/module_state.h"
#include "shell/thumbnail_api.h"
#include "shell/thumbnail_provider.h"
#include "shell/thumbnail_registry.h"

#include <windows.h>

#include <objbase.h>
#include <unknwn.h>

#include <atomic>
#include <cwchar>
#include <new>
#include <string>

namespace nv::shell {
namespace internal {

std::atomic<long> g_live_objects{0};
std::atomic<long> g_lock_refs{0};
std::atomic<long> g_active_decodes{0};
HINSTANCE g_instance = nullptr;

std::wstring ModulePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetModuleFileNameW(g_instance, path.data(),
                           static_cast<DWORD>(path.size()));
    if (length == 0) return std::wstring();
    // GetModuleFileNameW reports a truncated path by returning the buffer
    // size; grow until the terminating null fits.
    if (length < path.size()) {
      path.resize(length);
      return path;
    }
    if (path.size() >= 32768) return std::wstring();
    path.resize(path.size() * 2);
  }
}

class ThumbnailClassFactory final : public IClassFactory {
 public:
  ThumbnailClassFactory() { AddLiveObject(); }
  ~ThumbnailClassFactory() { ReleaseLiveObject(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return references_.fetch_add(1) + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = references_.fetch_sub(1) - 1;
    if (remaining == 0) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid,
                                           void** object) override {
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;
    return CreateThumbnailProvider(riid, object);
  }

  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    if (lock) {
      AddLockRef();
    } else {
      ReleaseLockRef();
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> references_{1};
};

}  // namespace internal

void AddLiveObject() { internal::g_live_objects.fetch_add(1); }
void ReleaseLiveObject() { internal::g_live_objects.fetch_sub(1); }
void AddLockRef() { internal::g_lock_refs.fetch_add(1); }
void ReleaseLockRef() { internal::g_lock_refs.fetch_sub(1); }
void BeginDecode() { internal::g_active_decodes.fetch_add(1); }
void EndDecode() { internal::g_active_decodes.fetch_sub(1); }

bool CanUnloadModule() {
  return internal::g_live_objects.load() == 0 &&
         internal::g_lock_refs.load() == 0 &&
         internal::g_active_decodes.load() == 0;
}

}  // namespace nv::shell

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    nv::shell::internal::g_instance = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid,
                                            void** object) {
  if (object == nullptr) return E_POINTER;
  *object = nullptr;
  if (!IsEqualCLSID(clsid, nv::shell::kThumbnailProviderClsid)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, IID_IClassFactory)) {
    return E_NOINTERFACE;
  }
  auto* factory = new (std::nothrow) nv::shell::internal::ThumbnailClassFactory();
  if (factory == nullptr) return E_OUTOFMEMORY;
  const HRESULT hr = factory->QueryInterface(riid, object);
  factory->Release();
  return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
  return nv::shell::CanUnloadModule() ? S_OK : S_FALSE;
}

extern "C" HRESULT WINAPI DllRegisterServer() {
  return nv::shell::RegisterThumbnailProvider(nv::shell::internal::ModulePath());
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
  return nv::shell::UnregisterThumbnailProvider();
}

extern "C" HRESULT WINAPI ZoreViewThumbnailRegister() {
  return nv::shell::RegisterThumbnailProvider(nv::shell::internal::ModulePath());
}

extern "C" HRESULT WINAPI ZoreViewThumbnailUnregister() {
  return nv::shell::UnregisterThumbnailProvider();
}

extern "C" BOOL WINAPI ZoreViewThumbnailIsRegistered() {
  const std::wstring path = nv::shell::internal::ModulePath();
  return path.empty()
             ? FALSE
             : (nv::shell::IsThumbnailProviderRegistered(path) ? TRUE : FALSE);
}

extern "C" DWORD WINAPI ZoreViewThumbnailGetPath(LPWSTR buffer,
                                                  DWORD capacity) {
  const std::wstring path = nv::shell::internal::ModulePath();
  if (path.empty()) return 0;
  const DWORD required = static_cast<DWORD>(path.size() + 1);
  if (buffer != nullptr && capacity >= required) {
    wmemcpy(buffer, path.c_str(), required);
  }
  return required;
}
