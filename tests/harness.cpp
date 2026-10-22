// End-to-end check of the plugin on DXVK, without the game.
//
// 1. Renders the game's own HDR blend shaders (vanilla, and Vanilla Plus Tonemapper's, as extracted by tools/sdp.py)
//    over a synthetic HDR scene: grey and coloured ramps from deep shadow to 65x over white, with and without bloom,
//    at several exposures, half of them masked for the alpha mask variant.
// 2. Loads the plugin the way xNVSE does, creates a device the way the game does (with CreateDevice hooked over the
//    plugin, as Windows' compatibility shims do on some launches) and renders the same through the shaders the plugin
//    swaps in. With SdrExact=1 they must match the originals. With HDR on they must still match wherever the
//    originals stayed below white, and go above white (up to the peak) where the originals clipped.
// 3. Triggers a frame trace and checks the plugin's decode: the scRGB frame it presents must be the gamma-space frame
//    in linear light, scaled to paper white and capped at the peak. After the present the back buffer must hold the
//    gamma-space frame again.
//
// usage: harness <DXVK d3d9.dll> <NewVegasHDR.dll> <folder with sdp017\ and vpt\ shaders> <work folder>
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "display.h"
#include "nvse.h"

namespace fs = std::filesystem;

namespace {

constexpr UINT kWidth = 256, kHeight = 48;  // test pattern
constexpr UINT kBackWidth = 640, kBackHeight = 360;
constexpr float kPeakNits = 1000, kPaperWhiteNits = 200;
constexpr float kGamma = 2.4f;  // not the default 2.2: the shaders and the decode must both follow the ini
constexpr float kTolerance = 2.0f / 1024;  // about one step of a 16-bit float near 1

struct Variant {
  const char* name;
};
const Variant kVariants[] = {{"ISHDRBLENDINSHADER"}, {"ISHDRBLENDINSHADERCIN"}, {"ISHDRBLENDINSHADERCINAM"}};
const char* const kLooks[] = {"sdp017", "vpt"};

int g_failures = 0;

// A stand-in for Windows' compatibility shims (apphelp.dll), which on some launches hook CreateDevice in the vtable
// that every IDirect3D9 shares, hook it again at every Direct3DCreate9, and call straight through to what they found
// first: anything hooked in that slot in between is skipped. The plugin has to see the device created anyway.
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
CreateDeviceFn g_shimFoundCreateDevice = nullptr;

HRESULT STDMETHODCALLTYPE shimCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** device) {
  return g_shimFoundCreateDevice(self, adapter, type, focus, flags, pp, device);
}

void shimHook(IDirect3D9* d3d) {
  void** vtable = *reinterpret_cast<void***>(d3d);
  if (!g_shimFoundCreateDevice)
    g_shimFoundCreateDevice = reinterpret_cast<CreateDeviceFn>(vtable[16]);
  DWORD protection;
  VirtualProtect(&vtable[16], sizeof(void*), PAGE_READWRITE, &protection);
  vtable[16] = reinterpret_cast<void*>(&shimCreateDevice);
  VirtualProtect(&vtable[16], sizeof(void*), protection, &protection);
}

void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok)
    ++g_failures;
}

std::vector<DWORD> readShader(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<DWORD> code((bytes.size() + 3) / 4);
  std::memcpy(code.data(), bytes.data(), bytes.size());
  return code;
}

float halfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  float v;
  if (exponent == 0)
    v = std::ldexp(static_cast<float>(mantissa), -24);
  else if (exponent == 31)
    v = mantissa ? NAN : INFINITY;
  else
    v = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
  return sign ? -v : v;
}

uint16_t floatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000;
  const int exponent = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = x & 0x7FFFFF;
  if (exponent <= 0)
    return static_cast<uint16_t>(sign);
  if (exponent >= 31)
    return static_cast<uint16_t>(sign | 0x7C00);
  uint32_t h = sign | (exponent << 10) | (mantissa >> 13);
  if (mantissa & 0x1000)  // round to nearest
    ++h;
  return static_cast<uint16_t>(h);
}

using Image = std::vector<float>;  // kWidth * kHeight * 4

// The synthetic scene and the game's constants for it.
struct Inputs {
  IDirect3DTexture9* scene = nullptr;
  IDirect3DTexture9* bloom = nullptr;
};

void fill(IDirect3DTexture9* texture, bool bloom) {
  static const float kTints[6][3] = {{1, 1, 1},       {1, 0.7f, 0.4f}, {1, 0.1f, 0.05f},
                                     {0.1f, 1, 0.1f}, {0.08f, 0.1f, 1}, {0.1f, 0.9f, 1}};
  static const float kAdapted[4] = {0.15f, 0.15f, 0.6f, 2.0f};
  D3DLOCKED_RECT locked;
  texture->LockRect(0, &locked, nullptr, 0);
  for (UINT y = 0; y < kHeight; ++y) {
    auto* row = reinterpret_cast<uint16_t*>(static_cast<char*>(locked.pBits) + y * locked.Pitch);
    const float* tint = kTints[(y / 2) % 6];
    const UINT group = y / 12;
    for (UINT x = 0; x < kWidth; ++x) {
      const float v = 0.002f * std::exp2(x / 16.0f);
      float rgba[4];
      for (int c = 0; c < 3; ++c)
        rgba[c] = bloom ? (group % 2 ? 0.3f * v * tint[c] : 0) : v * tint[c];
      rgba[3] = bloom ? kAdapted[group] : (y % 2 ? 0.0f : 1.0f);
      for (int c = 0; c < 4; ++c)
        row[x * 4 + c] = floatToHalf(rgba[c]);
    }
  }
  texture->UnlockRect(0);
}

// Renders one HDR blend pass over the test pattern into a 16-bit float target and reads it back.
class Renderer {
 public:
  explicit Renderer(IDirect3DDevice9* device) : device_(device) {
    device->CreateTexture(kWidth, kHeight, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &scene_, nullptr);
    device->CreateTexture(kWidth, kHeight, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &bloom_, nullptr);
    fill(scene_, false);
    fill(bloom_, true);
    device->CreateTexture(kWidth, kHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &target_,
                          nullptr);
    device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &readback_, nullptr);
    const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
                                          {0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
                                          D3DDECL_END()};
    device->CreateVertexDeclaration(elements, &decl_);
  }

  ~Renderer() {
    for (IUnknown* u : std::initializer_list<IUnknown*>{scene_, bloom_, target_, readback_, decl_})
      if (u)
        u->Release();
  }

  bool ok() const { return scene_ && bloom_ && target_ && readback_ && decl_; }
  bool constantsKept() const { return constantsKept_; }

  Image render(IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps) {
    IDirect3DSurface9* surface = nullptr;
    target_->GetSurfaceLevel(0, &surface);
    device_->SetRenderTarget(0, surface);
    D3DVIEWPORT9 viewport = {0, 0, kWidth, kHeight, 0, 1};
    device_->SetViewport(&viewport);
    device_->SetRenderState(D3DRS_ZENABLE, FALSE);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device_->SetTexture(0, bloom_);
    device_->SetTexture(1, scene_);
    for (DWORD s = 0; s < 2; ++s) {
      device_->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      device_->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      device_->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      device_->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      device_->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    // The game's image space vertex shader: c0 shifts the quad by half a pixel, c1 and c2 offset the two texcoords.
    const float vsConstants[3][4] = {{0.5f / kWidth, 0.5f / kHeight, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    device_->SetVertexShaderConstantF(0, vsConstants[0], 3);
    const float hdrParam[4] = {0.5f, 0, 0, 0}, blurScale[4] = {0, 0, 1, 0};
    const float cinematic[4] = {1.15f, 0.5f, 1.08f, 0.95f}, tint[4] = {1, 0.85f, 0.7f, 0.15f};
    const float fade[4] = {0.1f, 0.1f, 0.1f, 0.05f}, alphaMask[4] = {0, 0, 0, 0.6f};
    device_->SetPixelShaderConstantF(1, hdrParam, 1);
    device_->SetPixelShaderConstantF(2, blurScale, 1);
    device_->SetPixelShaderConstantF(19, cinematic, 1);
    device_->SetPixelShaderConstantF(20, tint, 1);
    device_->SetPixelShaderConstantF(22, fade, 1);
    device_->SetPixelShaderConstantF(23, alphaMask, 1);
    // Values of the game's own in the registers the plugin borrows; they have to be back after the draw.
    const float borrowed[2][4] = {{7, 6, 5, 4}, {3, 2, 1, 0}};
    device_->SetPixelShaderConstantF(24, borrowed[0], 2);
    device_->SetVertexDeclaration(decl_);
    device_->SetVertexShader(vs);
    device_->SetPixelShader(ps);
    const float quad[4][6] = {
        {-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};
    device_->BeginScene();
    device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]);
    device_->EndScene();
    float after[2][4] = {};
    device_->GetPixelShaderConstantF(24, after[0], 2);
    constantsKept_ &= std::memcmp(after, borrowed, sizeof after) == 0;
    device_->GetRenderTargetData(surface, readback_);
    surface->Release();
    Image image(kWidth * kHeight * 4);
    D3DLOCKED_RECT locked;
    readback_->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    for (UINT y = 0; y < kHeight; ++y) {
      const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(locked.pBits) + y * locked.Pitch);
      for (UINT i = 0; i < kWidth * 4; ++i)
        image[y * kWidth * 4 + i] = halfToFloat(row[i]);
    }
    readback_->UnlockRect();
    return image;
  }

 private:
  IDirect3DDevice9* device_;
  IDirect3DTexture9* scene_ = nullptr;
  IDirect3DTexture9* bloom_ = nullptr;
  IDirect3DTexture9* target_ = nullptr;
  IDirect3DSurface9* readback_ = nullptr;
  IDirect3DVertexDeclaration9* decl_ = nullptr;
  bool constantsKept_ = true;
};

HWND makeWindow() {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"NewVegasHDRHarness";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"NewVegasHDR harness", WS_POPUP,
                              -20000, -20000, kBackWidth, kBackHeight, nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  return hwnd;
}

// The game's way: windowed, X8R8G8B8, no automatic depth buffer.
IDirect3DDevice9* createDevice(IDirect3D9* d3d, HWND hwnd) {
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = kBackWidth;
  pp.BackBufferHeight = kBackHeight;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9* device = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                 &device);
  if (FAILED(hr))
    std::printf("CreateDevice failed: 0x%08lx\n", hr);
  return device;
}

struct Shaders {
  IDirect3DVertexShader9* vs = nullptr;
  IDirect3DPixelShader9* ps = nullptr;
  UINT psSize = 0;  // of the bytecode the device holds: tells ours from the game's
};

Shaders createShaders(IDirect3DDevice9* device, const fs::path& dir, const char* look, const char* variant) {
  Shaders s;
  const fs::path base = dir / look / variant;
  std::vector<DWORD> vso = readShader(base.string() + ".vso"), pso = readShader(base.string() + ".pso");
  device->CreateVertexShader(vso.data(), &s.vs);
  device->CreatePixelShader(pso.data(), &s.ps);
  if (s.ps)
    s.ps->GetFunction(nullptr, &s.psSize);
  return s;
}

void release(Shaders& s) {
  if (s.vs)
    s.vs->Release();
  if (s.ps)
    s.ps->Release();
  s = {};
}

void writeIni(const fs::path& path, bool sdrExact) {
  std::ofstream f(path);
  f << "[HDR]\nEnabled=1\nPeakNits=" << kPeakNits << "\nPaperWhiteNits=" << kPaperWhiteNits
    << "\nShoulderStart=0.5\nGamma=" << kGamma << "\n[Advanced]\nSdrExact=" << (sdrExact ? 1 : 0)
    << "\nForceHdr=1\n";
}

// Largest difference between an original (clipped to [0, 1] as the game's 8-bit target does) and ours. Prints where
// it is when it is over the tolerance.
float maxDifference(const Image& original, const Image& ours, bool onlyBelowWhite) {
  float worst = 0;
  size_t worstAt = 0;
  for (size_t p = 0; p < original.size(); p += 4) {
    bool belowWhite = true;
    for (int c = 0; c < 3; ++c)
      belowWhite &= original[p + c] < 0.995f;
    if (onlyBelowWhite && !belowWhite)
      continue;
    for (int c = 0; c < 4; ++c) {
      const float d = std::fabs(std::clamp(original[p + c], 0.0f, 1.0f) - ours[p + c]);
      if (d > worst)
        worst = d, worstAt = p + c;
    }
  }
  if (worst > kTolerance) {
    const size_t p = worstAt / 4 * 4;
    std::printf("         worst at x %zu y %zu channel %zu: original %.4f %.4f %.4f %.4f, ours %.4f %.4f %.4f %.4f\n",
                p / 4 % kWidth, p / 4 / kWidth, worstAt % 4, original[p], original[p + 1], original[p + 2],
                original[p + 3], ours[p], ours[p + 1], ours[p + 2], ours[p + 3]);
  }
  return worst;
}

struct Dds {
  UINT width = 0, height = 0;
  std::vector<float> rgba;
};

Dds readDds(const fs::path& path) {
  Dds d;
  std::ifstream f(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0)
    return d;
  DWORD header[31];
  std::memcpy(header, bytes.data() + 4, sizeof header);
  d.height = header[2];
  d.width = header[3];
  const DWORD fourCC = header[20];
  if (fourCC != D3DFMT_A16B16G16R16F || bytes.size() < 128 + size_t(d.width) * d.height * 8)
    return {};
  d.rgba.resize(size_t(d.width) * d.height * 4);
  const auto* half = reinterpret_cast<const uint16_t*>(bytes.data() + 128);
  for (size_t i = 0; i < d.rgba.size(); ++i)
    d.rgba[i] = halfToFloat(half[i]);
  return d;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::printf("usage: harness <DXVK d3d9.dll> <NewVegasHDR.dll> <extracted shader folder> <work folder>\n");
    return 2;
  }
  const fs::path dxvkPath = argv[1], pluginSource = argv[2], shaderDir = argv[3], work = argv[4];
  fs::create_directories(work / "plugin");
  fs::remove_all(work / "captures");
  fs::create_directories(work / "captures");

  if (!LoadLibraryW(dxvkPath.c_str())) {
    std::printf("cannot load %s\n", dxvkPath.string().c_str());
    return 1;
  }
  using CreateFn = IDirect3D9*(WINAPI*)(UINT);
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(GetModuleHandleW(L"d3d9.dll"), "Direct3DCreate9"));
  HWND hwnd = makeWindow();
  DisplayInfo display = queryDisplay(hwnd);
  std::printf("display: Windows HDR %s, peak %.0f nits\n", display.hdr ? "on" : "off", display.maxNits);

  // 1. The originals, before the plugin is loaded.
  std::printf("originals:\n");
  Image reference[2][3];
  {
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    shimHook(d3d);
    IDirect3DDevice9* device = createDevice(d3d, hwnd);
    if (!device)
      return 1;
    Renderer renderer(device);
    for (int l = 0; l < 2; ++l) {
      for (int v = 0; v < 3; ++v) {
        Shaders s = createShaders(device, shaderDir, kLooks[l], kVariants[v].name);
        if (!s.vs || !s.ps) {
          std::printf("cannot create %s %s\n", kLooks[l], kVariants[v].name);
          return 1;
        }
        reference[l][v] = renderer.render(s.vs, s.ps);
        float top = 0;
        for (float f : reference[l][v])
          top = std::max(top, f);
        std::printf("  %-6s %-24s ps %u B, brightest output %.2f\n", kLooks[l], kVariants[v].name, s.psSize, top);
        release(s);
      }
    }
    device->Release();
    d3d->Release();
  }

  // 2. Load the plugin as xNVSE does.
  const fs::path plugin = work / "plugin" / "NewVegasHDR.dll";
  fs::copy_file(pluginSource, plugin, fs::copy_options::overwrite_existing);
  const fs::path ini = work / "plugin" / "NewVegasHDR.ini";
  writeIni(ini, true);
  HMODULE module = LoadLibraryW(plugin.c_str());
  using QueryFn = bool (*)(const nvse::Interface*, nvse::PluginInfo*);
  using LoadFn = bool (*)(const nvse::Interface*);
  auto query = module ? reinterpret_cast<QueryFn>(GetProcAddress(module, "NVSEPlugin_Query")) : nullptr;
  auto load = module ? reinterpret_cast<LoadFn>(GetProcAddress(module, "NVSEPlugin_Load")) : nullptr;
  if (!query || !load) {
    std::printf("cannot load the plugin or find its exports\n");
    return 1;
  }
  const nvse::Interface nvse = {0x06040080, 0x040020D0, 0, 0};
  nvse::PluginInfo info = {};
  check(query(&nvse, &info) && info.name && std::string(info.name) == "NewVegasHDR", "NVSEPlugin_Query");
  check(load(&nvse), "NVSEPlugin_Load");

  IDirect3D9* d3d = create(D3D_SDK_VERSION);
  shimHook(d3d);  // as the shim does at every Direct3DCreate9, after the plugin has loaded
  IDirect3DDevice9* device = createDevice(d3d, hwnd);
  if (!device)
    return 1;
  IDirect3DSurface9* backBuffer = nullptr;
  device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
  D3DSURFACE_DESC bbDesc = {};
  backBuffer->GetDesc(&bbDesc);
  check(bbDesc.Format == D3DFMT_A16B16G16R16F,
        "the back buffer became A16B16G16R16F, with CreateDevice hooked over the plugin as Windows' shims do");

  Renderer renderer(device);
  Shaders shaders[2][3];
  std::printf("with the plugin, SdrExact=1:\n");
  for (int l = 0; l < 2; ++l) {
    for (int v = 0; v < 3; ++v) {
      shaders[l][v] = createShaders(device, shaderDir, kLooks[l], kVariants[v].name);
      const Image ours = renderer.render(shaders[l][v].vs, shaders[l][v].ps);
      const float diff = maxDifference(reference[l][v], ours, false);
      char what[160];
      std::snprintf(what, sizeof what, "%-6s %-24s replaced (ps %u B), largest difference %.5f", kLooks[l],
                    kVariants[v].name, shaders[l][v].psSize, diff);
      check(diff <= kTolerance, what);
    }
  }

  // HDR: rewrite the ini; the plugin reads it again within 60 frames.
  writeIni(ini, false);
  for (int i = 0; i < 61; ++i)
    device->Present(nullptr, nullptr, nullptr, nullptr);
  std::printf("with the plugin, HDR:\n");
  const float peakGamma = std::pow(kPeakNits / kPaperWhiteNits, 1 / kGamma);
  for (int l = 0; l < 2; ++l) {
    for (int v = 0; v < 3; ++v) {
      const Image ours = renderer.render(shaders[l][v].vs, shaders[l][v].ps);
      const float diff = maxDifference(reference[l][v], ours, true);
      int above = 0;
      float top = 0;
      for (size_t p = 0; p < ours.size(); p += 4)
        for (int c = 0; c < 3; ++c) {
          top = std::max(top, ours[p + c]);
          above += ours[p + c] > 1.001f;
        }
      char what[200];
      std::snprintf(what, sizeof what,
                    "%-6s %-24s below white: largest difference %.5f; %d values above white, brightest %.3f (peak %.3f)",
                    kLooks[l], kVariants[v].name, diff, above, top, peakGamma);
      check(diff <= kTolerance && above > 0 && top <= peakGamma * 1.002f, what);
    }
  }
  check(renderer.constantsKept(), "the game's own c24 and c25 are back after each of our draws");

  // 3. Trace a frame: the plugin saves the frame before and after its decode.
  std::printf("decode:\n");
  {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::ofstream trigger(fs::path(exe).parent_path() / "NewVegasHDR.trace", std::ios::binary);
    const std::u8string dir = fs::absolute(work / "captures").u8string();
    trigger.write(reinterpret_cast<const char*>(dir.data()), static_cast<std::streamsize>(dir.size()));
    trigger << "\n";
  }
  IDirect3DSurface9* ramp = nullptr;
  device->CreateOffscreenPlainSurface(kBackWidth, kBackHeight, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &ramp, nullptr);
  {
    D3DLOCKED_RECT locked;
    ramp->LockRect(&locked, nullptr, 0);
    for (UINT y = 0; y < kBackHeight; ++y) {
      auto* row = reinterpret_cast<uint16_t*>(static_cast<char*>(locked.pBits) + y * locked.Pitch);
      for (UINT x = 0; x < kBackWidth; ++x) {
        const float r = 3.0f * x / (kBackWidth - 1);
        const float rgba[4] = {r, 0.5f * r, static_cast<float>(y) / (kBackHeight - 1), 1};
        for (int c = 0; c < 4; ++c)
          row[x * 4 + c] = floatToHalf(rgba[c]);
      }
    }
    ramp->UnlockRect();
  }
  for (int i = 0; i < 70; ++i) {
    renderer.render(shaders[1][1].vs, shaders[1][1].ps);
    device->UpdateSurface(ramp, nullptr, backBuffer, nullptr);
    device->Present(nullptr, nullptr, nullptr, nullptr);
  }
  {
    // After the present the back buffer must hold the frame as the game drew it, not the decoded one.
    IDirect3DSurface9* now = nullptr;
    IDirect3DSurface9* readback = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &now);
    device->CreateOffscreenPlainSurface(kBackWidth, kBackHeight, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &readback,
                                        nullptr);
    float worst = INFINITY;
    if (now && readback && SUCCEEDED(device->GetRenderTargetData(now, readback))) {
      D3DLOCKED_RECT got, drawn;
      readback->LockRect(&got, nullptr, D3DLOCK_READONLY);
      ramp->LockRect(&drawn, nullptr, D3DLOCK_READONLY);
      worst = 0;
      for (UINT y = 0; y < kBackHeight; ++y) {
        const auto* a = reinterpret_cast<const uint16_t*>(static_cast<const char*>(got.pBits) + y * got.Pitch);
        const auto* b = reinterpret_cast<const uint16_t*>(static_cast<const char*>(drawn.pBits) + y * drawn.Pitch);
        for (UINT i = 0; i < kBackWidth * 4; ++i)
          if (i % 4 != 3)
            worst = std::max(worst, std::fabs(halfToFloat(a[i]) - halfToFloat(b[i])));
      }
      ramp->UnlockRect();
      readback->UnlockRect();
    }
    if (readback)
      readback->Release();
    if (now)
      now->Release();
    char what[120];
    std::snprintf(what, sizeof what, "after the present the back buffer holds the game's frame: largest difference %.5f",
                  worst);
    check(worst <= kTolerance, what);
  }
  fs::path gammaFile, scrgbFile, traceFile;
  for (const auto& entry : fs::directory_iterator(work / "captures")) {
    const std::string name = entry.path().filename().string();
    if (name.ends_with("-frame-gamma.dds"))
      gammaFile = entry.path();
    else if (name.ends_with("-frame-scrgb.dds"))
      scrgbFile = entry.path();
    else if (name.starts_with("trace-"))
      traceFile = entry.path();
  }
  check(!traceFile.empty(), "the trace was written");
  const Dds gamma = readDds(gammaFile), scrgb = readDds(scrgbFile);
  check(gamma.width == kBackWidth && scrgb.width == kBackWidth, "both frames were saved");
  if (gamma.width == kBackWidth && scrgb.width == kBackWidth) {
    float worst = 0, top = 0;
    for (size_t i = 0; i < gamma.rgba.size(); ++i) {
      if (i % 4 == 3)
        continue;
      const float expected =
          std::min(std::pow(std::max(gamma.rgba[i], 0.0f), kGamma) * kPaperWhiteNits / 80, kPeakNits / 80);
      worst = std::max(worst, std::fabs(scrgb.rgba[i] - expected) / std::max(expected, 0.01f));
      top = std::max(top, scrgb.rgba[i]);
    }
    char what[160];
    std::snprintf(what, sizeof what, "scRGB = linear gamma frame x paper white / 80: largest relative error %.4f, "
                  "brightest %.2f (peak %.2f)", worst, top, kPeakNits / 80);
    check(worst < 0.01f && top <= kPeakNits / 80 * 1.001f, what);
  }

  ramp->Release();
  backBuffer->Release();
  for (auto& look : shaders)
    for (auto& s : look)
      release(s);
  device->Release();
  d3d->Release();
  DestroyWindow(hwnd);
  std::printf(g_failures ? "%d check(s) failed\n" : "all checks passed\n", g_failures);
  return g_failures ? 1 : 0;
}
