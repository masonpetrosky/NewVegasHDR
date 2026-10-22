#include "masquerade.h"

#include <intrin.h>

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "log.h"
#include "state.h"
#include "vtable.h"

namespace masquerade {
namespace {

// Private data that holds the D3DFORMAT the game asked for: {ecd28a52-d5b9-4ac8-82b3-6775009db2e9}.
constexpr GUID kShownFormat = {0xecd28a52, 0xd5b9, 0x4ac8, {0x82, 0xb3, 0x67, 0x75, 0x00, 0x9d, 0xb2, 0xe9}};

// The only format the plugin gives the surfaces it changes.
constexpr D3DFORMAT kRealFormat = D3DFMT_A16B16G16R16F;

// Logs the first time each call site sees a changed format, so the log tells what reads them.
void note(const char* call, const void* resource, D3DFORMAT shown, const void* caller) {
  static std::atomic<int> logged = 0;
  static std::mutex mutex;
  static std::vector<std::pair<const char*, const void*>> seen;
  if (logged.load(std::memory_order_relaxed) >= 32)
    return;
  std::lock_guard lock(mutex);
  for (const auto& [c, at] : seen)
    if (c == call && at == caller)
      return;
  seen.emplace_back(call, caller);
  ++logged;
  LOG("%s from %s: %p shown as %s%s", call, logging::where(caller).c_str(), resource, formatName(shown),
      resource == g.backBuffer || resource == g.swapChain ? " (back buffer)" : "");
}

// A surface the conversions go through, made again when the size or format they need changes.
struct Scratch {
  IDirect3DSurface9* surface = nullptr;
  UINT width = 0, height = 0;
  D3DFORMAT format = D3DFMT_UNKNOWN;

  IDirect3DSurface9* get(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT f, D3DPOOL pool) {
    if (surface && (width != w || height != h || format != f))
      release();
    if (!surface) {
      const HRESULT hr =
          pool == D3DPOOL_DEFAULT
              ? g.d3d.createRenderTarget(device, w, h, f, D3DMULTISAMPLE_NONE, 0, FALSE, &surface, nullptr)
              : device->CreateOffscreenPlainSurface(w, h, f, D3DPOOL_SYSTEMMEM, &surface, nullptr);
      if (FAILED(hr)) {
        LOG("cannot create a %ux%u %s %s for a format conversion (0x%08lx)", w, h, formatName(f),
            pool == D3DPOOL_DEFAULT ? "render target" : "system memory surface", hr);
        surface = nullptr;
        return nullptr;
      }
      width = w, height = h, format = f;
    }
    return surface;
  }

  void release() {
    if (surface)
      surface->Release();
    surface = nullptr;
  }
};

std::mutex g_mutex;  // guards everything below: the game's device is multithreaded
Scratch g_lockTarget, g_lockMemory;  // a locked surface's 8-bit copy, on the GPU and in system memory
Scratch g_copyTarget;  // GetRenderTargetData and UpdateSurface conversions

// The surface the game holds locked through a converted copy. One at a time.
std::atomic<IDirect3DSurface9*> g_locked = nullptr;
DWORD g_lockFlags = 0;
RECT g_lockRect = {};

HRESULT lockConverted(IDirect3DSurface9* surface, const D3DSURFACE_DESC& desc, D3DFORMAT shown,
                      D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
  IDirect3DDevice9* device = nullptr;
  surface->GetDevice(&device);
  if (!device)
    return D3DERR_INVALIDCALL;
  std::lock_guard lock(g_mutex);
  HRESULT hr = D3DERR_INVALIDCALL;
  if (g_locked) {
    LOG("LockRect: %p is locked while %p still is; only one converted lock at a time", static_cast<void*>(surface),
        static_cast<void*>(g_locked.load()));
  } else {
    InternalScope internal;
    IDirect3DSurface9* target = g_lockTarget.get(device, desc.Width, desc.Height, shown, D3DPOOL_DEFAULT);
    IDirect3DSurface9* memory = g_lockMemory.get(device, desc.Width, desc.Height, shown, D3DPOOL_SYSTEMMEM);
    if (target && memory) {
      hr = g.d3d.stretchRect(device, surface, nullptr, target, nullptr, D3DTEXF_NONE);
      if (SUCCEEDED(hr))
        hr = g.d3d.getRenderTargetData(device, target, memory);
      if (SUCCEEDED(hr))
        hr = g.d3d.surfaceLockRect(memory, locked, rect, flags & (D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK));
      if (SUCCEEDED(hr)) {
        g_locked = surface;
        g_lockFlags = flags;
        g_lockRect = rect ? *rect : RECT{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
      } else {
        LOG("LockRect through an 8-bit copy failed (0x%08lx)", hr);
      }
    }
  }
  device->Release();
  return hr;
}

// Writes the locked rectangle back unless the lock was read-only: system memory to the 8-bit render target, then
// converted into the surface. Only the rectangle, so that highlights elsewhere in the surface stay.
HRESULT unlockConverted(IDirect3DSurface9* surface) {
  std::lock_guard lock(g_mutex);
  if (g_locked != surface)
    return g.d3d.surfaceUnlockRect(surface);
  InternalScope internal;
  const HRESULT hr = g.d3d.surfaceUnlockRect(g_lockMemory.surface);
  if (SUCCEEDED(hr) && !(g_lockFlags & D3DLOCK_READONLY)) {
    IDirect3DDevice9* device = nullptr;
    surface->GetDevice(&device);
    if (device) {
      const POINT at = {g_lockRect.left, g_lockRect.top};
      if (SUCCEEDED(g.d3d.updateSurface(device, g_lockMemory.surface, &g_lockRect, g_lockTarget.surface, &at)))
        g.d3d.stretchRect(device, g_lockTarget.surface, &g_lockRect, surface, &g_lockRect, D3DTEXF_NONE);
      device->Release();
    }
  }
  g_locked = nullptr;
  return hr;
}

HRESULT STDMETHODCALLTYPE hookSurfaceGetDesc(IDirect3DSurface9* self, D3DSURFACE_DESC* desc) {
  const HRESULT hr = g.d3d.surfaceGetDesc(self, desc);
  if (SUCCEEDED(hr) && desc->Format == kRealFormat && !g.internal) {
    if (const D3DFORMAT shown = shownFormat(self); shown != D3DFMT_UNKNOWN) {
      desc->Format = shown;
      note("GetDesc", self, shown, _ReturnAddress());
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hookTextureGetLevelDesc(IDirect3DTexture9* self, UINT level, D3DSURFACE_DESC* desc) {
  const HRESULT hr = g.d3d.textureGetLevelDesc(self, level, desc);
  if (SUCCEEDED(hr) && desc->Format == kRealFormat && !g.internal) {
    if (const D3DFORMAT shown = shownFormat(self); shown != D3DFMT_UNKNOWN) {
      desc->Format = shown;
      note("GetLevelDesc", self, shown, _ReturnAddress());
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hookSwapChainGetPresentParameters(IDirect3DSwapChain9* self, D3DPRESENT_PARAMETERS* pp) {
  const HRESULT hr = g.d3d.swapChainGetPresentParameters(self, pp);
  if (SUCCEEDED(hr) && pp->BackBufferFormat == kRealFormat && self == g.swapChain && g.fp16BackBuffer &&
      !g.internal) {
    pp->BackBufferFormat = g.gameFormat;
    note("GetPresentParameters", self, g.gameFormat, _ReturnAddress());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hookSurfaceLockRect(IDirect3DSurface9* self, D3DLOCKED_RECT* locked, const RECT* rect,
                                              DWORD flags) {
  D3DSURFACE_DESC desc;
  if (!g.internal && locked && SUCCEEDED(g.d3d.surfaceGetDesc(self, &desc)) && desc.Format == kRealFormat) {
    if (const D3DFORMAT shown = shownFormat(self); shown != D3DFMT_UNKNOWN) {
      note("LockRect", self, shown, _ReturnAddress());
      return lockConverted(self, desc, shown, locked, rect, flags);
    }
  }
  return g.d3d.surfaceLockRect(self, locked, rect, flags);
}

HRESULT STDMETHODCALLTYPE hookSurfaceUnlockRect(IDirect3DSurface9* self) {
  if (!g.internal && self == g_locked.load())
    return unlockConverted(self);
  return g.d3d.surfaceUnlockRect(self);
}

}  // namespace

void install(IDirect3DDevice9* device) {
  InternalScope internal;
  IDirect3DSurface9* backBuffer = nullptr;
  if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
    vt::hook(backBuffer, vt::kSurfaceGetDesc, &hookSurfaceGetDesc, g.d3d.surfaceGetDesc);
    vt::hook(backBuffer, vt::kSurfaceLockRect, &hookSurfaceLockRect, g.d3d.surfaceLockRect);
    vt::hook(backBuffer, vt::kSurfaceUnlockRect, &hookSurfaceUnlockRect, g.d3d.surfaceUnlockRect);
    backBuffer->Release();
  }
  IDirect3DTexture9* texture = nullptr;
  if (SUCCEEDED(g.d3d.createTexture(device, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &texture, nullptr))) {
    vt::hook(texture, vt::kTextureGetLevelDesc, &hookTextureGetLevelDesc, g.d3d.textureGetLevelDesc);
    texture->Release();
  }
  IDirect3DSwapChain9* swapChain = nullptr;
  if (SUCCEEDED(device->GetSwapChain(0, &swapChain))) {
    vt::hook(swapChain, vt::kSwapChainGetPresentParameters, &hookSwapChainGetPresentParameters,
             g.d3d.swapChainGetPresentParameters);
    swapChain->Release();
  }
  const bool ok = g.d3d.surfaceGetDesc && g.d3d.surfaceLockRect && g.d3d.surfaceUnlockRect &&
                  g.d3d.textureGetLevelDesc && g.d3d.swapChainGetPresentParameters;
  if (ok)
    LOG("the game sees the formats it asked for (GetDesc, GetLevelDesc, GetPresentParameters and LockRect hooked)");
  else
    LOG("could not hook every method that reports a format; the game may see the 16-bit float ones");
}

void tag(IDirect3DResource9* resource, D3DFORMAT shown) {
  resource->SetPrivateData(kShownFormat, &shown, sizeof shown, 0);
}

void tagTexture(IDirect3DTexture9* texture, D3DFORMAT shown) {
  tag(texture, shown);
  for (DWORD level = 0; level < texture->GetLevelCount(); ++level) {
    IDirect3DSurface9* surface = nullptr;
    if (SUCCEEDED(texture->GetSurfaceLevel(level, &surface))) {
      tag(surface, shown);
      surface->Release();
    }
  }
}

void tagBackBuffers(IDirect3DDevice9* device, D3DFORMAT shown) {
  UINT count = 1;
  IDirect3DSwapChain9* swapChain = nullptr;
  if (SUCCEEDED(device->GetSwapChain(0, &swapChain))) {
    D3DPRESENT_PARAMETERS pp = {};
    if (SUCCEEDED(swapChain->GetPresentParameters(&pp)) && pp.BackBufferCount)
      count = pp.BackBufferCount;
    swapChain->Release();
  }
  for (UINT i = 0; i < count; ++i) {
    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, i, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
      break;
    tag(backBuffer, shown);
    backBuffer->Release();
  }
}

D3DFORMAT shownFormat(IDirect3DResource9* resource) {
  D3DFORMAT format = D3DFMT_UNKNOWN;
  DWORD size = sizeof format;
  if (!resource || FAILED(resource->GetPrivateData(kShownFormat, &format, &size)) || size != sizeof format)
    return D3DFMT_UNKNOWN;
  return format;
}

HRESULT realDesc(IDirect3DSurface9* surface, D3DSURFACE_DESC* desc) {
  return g.d3d.surfaceGetDesc ? g.d3d.surfaceGetDesc(surface, desc) : surface->GetDesc(desc);
}

bool getRenderTargetData(IDirect3DDevice9* device, IDirect3DSurface9* source, IDirect3DSurface9* dest, HRESULT& hr,
                         const void* caller) {
  D3DSURFACE_DESC from, to;
  if (!source || !dest || FAILED(realDesc(source, &from)) || FAILED(realDesc(dest, &to)) ||
      from.Format == to.Format || from.Width != to.Width || from.Height != to.Height ||
      to.Pool != D3DPOOL_SYSTEMMEM || shownFormat(source) == D3DFMT_UNKNOWN)
    return false;
  note("GetRenderTargetData", source, to.Format, caller);
  std::lock_guard lock(g_mutex);
  InternalScope internal;
  IDirect3DSurface9* target = g_copyTarget.get(device, to.Width, to.Height, to.Format, D3DPOOL_DEFAULT);
  if (!target)
    return false;
  hr = g.d3d.stretchRect(device, source, nullptr, target, nullptr, D3DTEXF_NONE);
  if (SUCCEEDED(hr))
    hr = g.d3d.getRenderTargetData(device, target, dest);
  return true;
}

bool updateSurface(IDirect3DDevice9* device, IDirect3DSurface9* source, const RECT* rect, IDirect3DSurface9* dest,
                   const POINT* point, HRESULT& hr, const void* caller) {
  D3DSURFACE_DESC from, to;
  if (!source || !dest || FAILED(realDesc(source, &from)) || FAILED(realDesc(dest, &to)) ||
      from.Format == to.Format || from.Pool != D3DPOOL_SYSTEMMEM || shownFormat(dest) == D3DFMT_UNKNOWN)
    return false;
  note("UpdateSurface", dest, from.Format, caller);
  std::lock_guard lock(g_mutex);
  InternalScope internal;
  IDirect3DSurface9* target = g_copyTarget.get(device, to.Width, to.Height, from.Format, D3DPOOL_DEFAULT);
  if (!target)
    return false;
  // The same copy into an 8-bit render target of the destination's size (DXVK puts it at the point, or at 0, 0), then
  // just that rectangle converted into the destination.
  hr = g.d3d.updateSurface(device, source, rect, target, point);
  if (SUCCEEDED(hr)) {
    const LONG x = point ? point->x : 0, y = point ? point->y : 0;
    const LONG w = rect ? rect->right - rect->left : static_cast<LONG>(from.Width);
    const LONG h = rect ? rect->bottom - rect->top : static_cast<LONG>(from.Height);
    const RECT area = {x, y, x + w, y + h};
    hr = g.d3d.stretchRect(device, target, &area, dest, &area, D3DTEXF_NONE);
  }
  return true;
}

void releaseResources() {
  std::lock_guard lock(g_mutex);
  if (g_locked) {
    InternalScope internal;
    g.d3d.surfaceUnlockRect(g_lockMemory.surface);
    g_locked = nullptr;
  }
  g_lockTarget.release();
  g_lockMemory.release();
  g_copyTarget.release();
}

}  // namespace masquerade
