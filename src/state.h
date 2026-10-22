// The plugin's shared state: the game's device, the D3D9 methods the hooks replaced, and what the hooks track.
#pragma once

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "display.h"
#include "shader_info.h"

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetGammaRampFn = void(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, const D3DGAMMARAMP*);
using CreateTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                    IDirect3DTexture9**, HANDLE*);
using CreateRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE,
                                                         DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using UpdateSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                                    IDirect3DSurface9*, const POINT*);
using GetRenderTargetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
using StretchRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                                  IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using ColorFillFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepthStencilSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
using SceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using ClearFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT,
                                                           UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                             const void*, D3DFORMAT, const void*, UINT);
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                       const RGNDATA*, DWORD);

struct Originals {
  CreateDeviceFn createDevice = nullptr;
  CreateDeviceExFn createDeviceEx = nullptr;
  ResetFn reset = nullptr;
  PresentFn present = nullptr;
  SetGammaRampFn setGammaRamp = nullptr;
  CreateTextureFn createTexture = nullptr;
  CreateRenderTargetFn createRenderTarget = nullptr;
  UpdateSurfaceFn updateSurface = nullptr;
  GetRenderTargetDataFn getRenderTargetData = nullptr;
  StretchRectFn stretchRect = nullptr;
  ColorFillFn colorFill = nullptr;
  SetRenderTargetFn setRenderTarget = nullptr;
  SetDepthStencilSurfaceFn setDepthStencilSurface = nullptr;
  SceneFn beginScene = nullptr;
  SceneFn endScene = nullptr;
  ClearFn clear = nullptr;
  DrawPrimitiveFn drawPrimitive = nullptr;
  DrawIndexedPrimitiveFn drawIndexedPrimitive = nullptr;
  DrawPrimitiveUPFn drawPrimitiveUP = nullptr;
  DrawIndexedPrimitiveUPFn drawIndexedPrimitiveUP = nullptr;
  CreateVertexShaderFn createVertexShader = nullptr;
  SetVertexShaderFn setVertexShader = nullptr;
  CreatePixelShaderFn createPixelShader = nullptr;
  SetPixelShaderFn setPixelShader = nullptr;
  SetPixelShaderConstantFFn setPixelShaderConstantF = nullptr;
  SwapChainPresentFn swapChainPresent = nullptr;
};

// One of the game's HDR blend shaders as the game sees it: ours if it was replaced.
struct HdrBlendShader {
  IDirect3DPixelShader9* shader = nullptr;
  HdrBlend kind = HdrBlend::None;
  bool vanillaPlus = false;  // the game's shader had Vanilla Plus Tonemapper's curve
  bool replaced = false;
};

struct State {
  std::wstring gameDir;  // with a trailing backslash
  std::wstring iniPath;
  Config config;
  FILETIME iniTime = {};
  bool dxvk = false;
  Originals d3d;

  IDirect3DDevice9* device = nullptr;
  UINT width = 0, height = 0;  // back buffer
  D3DFORMAT gameFormat = D3DFMT_UNKNOWN;  // the back buffer format the game asked for
  bool fp16BackBuffer = false;
  bool scrgb = false;  // the swap chain takes scRGB: decode the frame before presenting it
  DisplayInfo display;
  uint64_t frame = 0;

  // The encode pass.
  IDirect3DTexture9* frameCopy = nullptr;
  IDirect3DVertexShader9* encodeVs = nullptr;
  IDirect3DPixelShader9* encodePs = nullptr;
  IDirect3DVertexDeclaration9* encodeDecl = nullptr;
  IDirect3DStateBlock9* stateBlock = nullptr;

  std::mutex mutex;  // guards writers of the containers below: resources may be created on loader threads
  // Append-only, read without the lock on every draw: an entry is complete before the count covers it.
  HdrBlendShader hdrBlends[16];
  std::atomic<int> hdrBlendCount = 0;
  std::unordered_map<const void*, ShaderInfo> shaders;  // every shader the game created, for traces
  std::atomic<int> upgradedTargets = 0;
  IDirect3DSurface9* backBuffer = nullptr;  // identity only, not referenced

  // Tracked device state (render thread only).
  IDirect3DSurface9* renderTarget0 = nullptr;
  IDirect3DPixelShader9* pixelShader = nullptr;
  IDirect3DVertexShader9* vertexShader = nullptr;
  float psConstants[32][4] = {};
  uint32_t drawsSincePresent = 0;  // a Present that never reaches the plugin shows up as draws without end
  // > 0 while this thread runs the plugin's own device calls: the hooks pass those through untouched.
  static inline thread_local int internal = 0;

  float peakNits() const;
  const HdrBlendShader* findHdrBlend(const IDirect3DPixelShader9* shader);
};

extern State g;

// Marks the plugin's own device calls for the duration of a scope.
struct InternalScope {
  InternalScope() { ++g.internal; }
  ~InternalScope() { --g.internal; }
};

const char* formatName(D3DFORMAT format);
