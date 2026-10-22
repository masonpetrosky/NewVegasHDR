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
// 4. Checks what the game sees: the back buffer and the upgraded render targets must report the 8-bit formats the game
//    asked for, while really being 16-bit float, and locks and copies to system memory must work in those formats.
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
#include <limits>
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
    draw(vs, ps, surface, 1);
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

  // Draws the pass into the top left of `target`, with the game's output alpha (BlurScale.z) set to `alpha`.
  void draw(IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps, IDirect3DSurface9* target, float alpha) {
    device_->SetRenderTarget(0, target);
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
    const float hdrParam[4] = {0.5f, 0, 0, 0}, blurScale[4] = {0, 0, alpha, 0};
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

// The game's way, as the plugin logged it in game: windowed, two X8R8G8B8 back buffers, lockable, no automatic depth
// buffer, a multithreaded pure device. Only the present interval differs, so that the test runs fast.
IDirect3DDevice9* createDevice(IDirect3D9* d3d, HWND hwnd) {
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = kBackWidth;
  pp.BackBufferHeight = kBackHeight;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 2;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9* device = nullptr;
  HRESULT hr = d3d->CreateDevice(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
      D3DCREATE_MULTITHREADED | D3DCREATE_PUREDEVICE | D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
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

// toneMapper null leaves the key out, for the plugin's default.
void writeIni(const fs::path& path, bool sdrExact, float highlightRange = 0, const char* toneMapper = "Auto") {
  std::ofstream f(path);
  f << "[HDR]\nEnabled=1\nPeakNits=" << kPeakNits << "\nPaperWhiteNits=" << kPaperWhiteNits
    << "\nHighlightRange=" << highlightRange << "\nShoulderStart=0.5\nGamma=" << kGamma << "\n";
  if (toneMapper)
    f << "ToneMapper=" << toneMapper << "\n";
  f << "[Advanced]\nSdrExact=" << (sdrExact ? 1 : 0) << "\nForceHdr=1\n";
}

// Rewrites the ini and presents until the plugin has read it again (it looks every 60 frames).
void applyIni(IDirect3DDevice9* device, const fs::path& path, bool sdrExact, float highlightRange = 0,
              const char* toneMapper = "Auto") {
  writeIni(path, sdrExact, highlightRange, toneMapper);
  for (int i = 0; i < 61; ++i)
    device->Present(nullptr, nullptr, nullptr, nullptr);
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

// Reads a render target back as 16-bit float RGBA, or returns nothing if it cannot. DXVK copies to system memory only
// between equal formats, so a result also proves that the surface really is 16-bit float, whatever it reports.
std::vector<float> readFp16(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
  std::vector<float> rgba;
  IDirect3DSurface9* sysmem = nullptr;
  if (SUCCEEDED(device->CreateOffscreenPlainSurface(kBackWidth, kBackHeight, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM,
                                                    &sysmem, nullptr)) &&
      SUCCEEDED(device->GetRenderTargetData(surface, sysmem))) {
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      rgba.resize(size_t(kBackWidth) * kBackHeight * 4);
      for (UINT y = 0; y < kBackHeight; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(locked.pBits) + y * locked.Pitch);
        for (UINT i = 0; i < kBackWidth * 4; ++i)
          rgba[size_t(y) * kBackWidth * 4 + i] = halfToFloat(row[i]);
      }
      sysmem->UnlockRect();
    }
  }
  if (sysmem)
    sysmem->Release();
  return rgba;
}

// The test ramp that goes into the back buffer: red rises from 0 to 3 across, green is half of it, blue rises down.
void rampColour(UINT x, UINT y, float (&rgb)[3]) {
  rgb[0] = 3.0f * x / (kBackWidth - 1);
  rgb[1] = 0.5f * rgb[0];
  rgb[2] = static_cast<float>(y) / (kBackHeight - 1);
}

// How far an X8R8G8B8 pixel (B, G, R, X in memory) is from the ramp clipped to 8 bits, in steps of 1/255.
int rampDifference8(const uint8_t* pixel, UINT x, UINT y) {
  float rgb[3];
  rampColour(x, y, rgb);
  int worst = 0;
  for (int c = 0; c < 3; ++c) {
    const int expected = static_cast<int>(std::lround(std::clamp(rgb[c], 0.0f, 1.0f) * 255));
    worst = std::max(worst, std::abs(pixel[2 - c] - expected));
  }
  return worst;
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
  check(!readFp16(device, backBuffer).empty(),
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

  // HDR with Vanilla Plus's curve as in SDR.
  applyIni(device, ini, false, 0);
  std::printf("with the plugin, HDR, HighlightRange=0:\n");
  const float peakGamma = std::pow(kPeakNits / kPaperWhiteNits, 1 / kGamma);
  Image exactHdr[2][3];
  for (int l = 0; l < 2; ++l) {
    for (int v = 0; v < 3; ++v) {
      const Image ours = renderer.render(shaders[l][v].vs, shaders[l][v].ps);
      exactHdr[l][v] = ours;
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

  // HDR with Vanilla Plus's curve stretched over the display's whole range: brighter in the highlights, no pixel darker
  // (by its brightest channel: the shoulder scales a highlight's channels together, so a weaker channel may drop a
  // little when the strongest grows), shadows (below a quarter of white) within 3 %, the peak still the limit. The
  // game's own look has no curve to stretch.
  applyIni(device, ini, false, 1);
  std::printf("with the plugin, HDR, HighlightRange=1:\n");
  for (int l = 0; l < 2; ++l) {
    for (int v = 0; v < 3; ++v) {
      const Image ours = renderer.render(shaders[l][v].vs, shaders[l][v].ps);
      const Image& exact = exactHdr[l][v];
      float darker = 0, changed = 0, shadowShift = 0, top = 0;
      int brighter = 0;
      for (size_t p = 0; p < ours.size(); p += 4) {
        const bool shadow = exact[p] < 0.25f && exact[p + 1] < 0.25f && exact[p + 2] < 0.25f;
        darker = std::max(darker, std::max({exact[p], exact[p + 1], exact[p + 2]}) -
                                      std::max({ours[p], ours[p + 1], ours[p + 2]}));
        for (int c = 0; c < 3; ++c) {
          changed = std::max(changed, std::fabs(ours[p + c] - exact[p + c]));
          brighter += ours[p + c] > exact[p + c] + 0.01f;
          top = std::max(top, ours[p + c]);
          if (shadow)
            shadowShift = std::max(shadowShift, std::fabs(ours[p + c] - exact[p + c]) / std::max(exact[p + c], 0.02f));
        }
      }
      char what[200];
      if (l == 0) {
        std::snprintf(what, sizeof what, "%-6s %-24s unchanged: largest difference %.5f", kLooks[l], kVariants[v].name,
                      changed);
        check(changed <= kTolerance, what);
      } else {
        std::snprintf(what, sizeof what,
                      "%-6s %-24s %d values brighter, none darker (%.5f), shadows within %.1f %%, brightest %.3f",
                      kLooks[l], kVariants[v].name, brighter, darker, shadowShift * 100, top);
        check(brighter > 0 && darker <= kTolerance && shadowShift < 0.03f && top <= peakGamma * 1.002f, what);
      }
    }
  }

  // ToneMapper picks the look of HDR output only: SdrExact (like SDR output) keeps Vanilla Plus's own image even with
  // ToneMapper=Vanilla, and HDR without the key (the default) gives the game's own look over Vanilla Plus's shaders.
  applyIni(device, ini, true, 1, "Vanilla");
  std::printf("with the plugin, SdrExact=1, ToneMapper=Vanilla:\n");
  for (int v = 0; v < 3; ++v) {
    const float diff = maxDifference(reference[1][v], renderer.render(shaders[1][v].vs, shaders[1][v].ps), false);
    char what[160];
    std::snprintf(what, sizeof what, "vpt    %-24s still Vanilla Plus's own: largest difference %.5f",
                  kVariants[v].name, diff);
    check(diff <= kTolerance, what);
  }
  applyIni(device, ini, false, 1, nullptr);
  std::printf("with the plugin, HDR, no ToneMapper key:\n");
  for (int v = 0; v < 3; ++v) {
    const Image ours = renderer.render(shaders[1][v].vs, shaders[1][v].ps);
    float diff = 0;
    for (size_t i = 0; i < ours.size(); ++i)
      diff = std::max(diff, std::fabs(ours[i] - exactHdr[0][v][i]));
    char what[160];
    std::snprintf(what, sizeof what, "vpt    %-24s gives the game's own look: largest difference %.5f",
                  kVariants[v].name, diff);
    check(diff <= kTolerance, what);
  }

  // The output alpha as the target the game asked for keeps it, also when the game leaves it unset (NaN).
  {
    IDirect3DTexture9* upgraded = nullptr;  // an 8-bit target the plugin makes 16-bit float
    IDirect3DTexture9* own = nullptr;  // a 16-bit float target of the game's own
    IDirect3DSurface9* upgradedLevel = nullptr;
    IDirect3DSurface9* ownLevel = nullptr;
    device->CreateTexture(kBackWidth, kBackHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                          &upgraded, nullptr);
    device->CreateTexture(kBackWidth, kBackHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
                          &own, nullptr);
    if (upgraded)
      upgraded->GetSurfaceLevel(0, &upgradedLevel);
    if (own)
      own->GetSurfaceLevel(0, &ownLevel);
    auto alphaLeft = [&](IDirect3DSurface9* target, float alpha) {
      if (!target)
        return -1.0f;
      renderer.draw(shaders[1][1].vs, shaders[1][1].ps, target, alpha);
      const std::vector<float> rgba = readFp16(device, target);
      return rgba.empty() ? -1.0f : rgba[(10 * size_t(kBackWidth) + 10) * 4 + 3];
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float back = alphaLeft(backBuffer, nan), upNan = alphaLeft(upgradedLevel, nan);
    const float upHigh = alphaLeft(upgradedLevel, 2.5f), mine = alphaLeft(ownLevel, 2.5f);
    char what[200];
    std::snprintf(what, sizeof what,
                  "output alpha: back buffer %g (1), 8-bit target %g and %g (0 for NaN, 1 for 2.5), the game's own "
                  "16-bit float target %g (2.5)",
                  back, upNan, upHigh, mine);
    check(back == 1 && upNan == 0 && upHigh == 1 && mine == 2.5f, what);
    for (IUnknown* u : std::initializer_list<IUnknown*>{upgradedLevel, ownLevel, upgraded, own})
      if (u)
        u->Release();
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
        float rgb[3];
        rampColour(x, y, rgb);
        const float rgba[4] = {rgb[0], rgb[1], rgb[2], 1};
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

  // 4. What the game sees. Gamebryo crashed at startup when the back buffer reported A16B16G16R16F.
  std::printf("formats the game sees:\n");
  {
    bool shown = true;
    for (UINT i = 0; i < 2; ++i) {
      IDirect3DSurface9* surface = nullptr;
      D3DSURFACE_DESC desc = {};
      shown &= SUCCEEDED(device->GetBackBuffer(0, i, D3DBACKBUFFER_TYPE_MONO, &surface)) &&
               SUCCEEDED(surface->GetDesc(&desc)) && desc.Format == D3DFMT_X8R8G8B8;
      if (surface)
        surface->Release();
    }
    check(shown, "both back buffers report X8R8G8B8 through GetDesc");
    IDirect3DSwapChain9* swapChain = nullptr;
    D3DPRESENT_PARAMETERS pp = {};
    device->GetSwapChain(0, &swapChain);
    check(swapChain && SUCCEEDED(swapChain->GetPresentParameters(&pp)) && pp.BackBufferFormat == D3DFMT_X8R8G8B8,
          "the swap chain's present parameters report X8R8G8B8");
    if (swapChain)
      swapChain->Release();
  }
  device->UpdateSurface(ramp, nullptr, backBuffer, nullptr);  // the ramp again, up to 3 times white
  {
    D3DLOCKED_RECT locked = {};
    int worst = 255;
    if (SUCCEEDED(backBuffer->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      worst = 0;
      for (UINT y = 0; y < kBackHeight; ++y)
        for (UINT x = 0; x < kBackWidth; ++x)
          worst = std::max(worst, rampDifference8(static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4,
                                                  x, y));
      backBuffer->UnlockRect();
    }
    char what[120];
    std::snprintf(what, sizeof what,
                  "a read-only lock of the back buffer gets the frame as X8R8G8B8: largest difference %d/255", worst);
    check(worst <= 1, what);
  }
  {
    // A lock that writes: the rectangle lands in the back buffer, and the rest keeps its values above white.
    const RECT rect = {8, 4, 24, 12};
    D3DLOCKED_RECT locked = {};
    const bool wrote = SUCCEEDED(backBuffer->LockRect(&locked, &rect, 0));
    if (wrote) {
      for (LONG y = 0; y < rect.bottom - rect.top; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(static_cast<char*>(locked.pBits) + y * locked.Pitch);
        for (LONG x = 0; x < rect.right - rect.left; ++x)
          row[x] = 0x00FF8000;  // red 255, green 128, blue 0
      }
      backBuffer->UnlockRect();
    }
    const std::vector<float> now = readFp16(device, backBuffer);
    auto at = [&](UINT x, UINT y, int c) { return now[(size_t(y) * kBackWidth + x) * 4 + c]; };
    float rgb[3];
    rampColour(kBackWidth - 1, 30, rgb);
    const bool ok = wrote && !now.empty() && std::fabs(at(10, 6, 0) - 1) < 0.002f &&
                    std::fabs(at(10, 6, 1) - 128.0f / 255) < 0.002f && at(10, 6, 2) < 0.002f &&
                    std::fabs(at(kBackWidth - 1, 30, 0) - rgb[0]) < kTolerance && at(30, 6, 0) > 0.1f;
    check(ok, "a lock that writes changes only its rectangle, and highlights elsewhere stay above white");
  }
  {
    IDirect3DSurface9* sysmem = nullptr;
    device->CreateOffscreenPlainSurface(kBackWidth, kBackHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
    int worst = 255;
    D3DLOCKED_RECT locked;
    if (sysmem && SUCCEEDED(device->GetRenderTargetData(backBuffer, sysmem)) &&
        SUCCEEDED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      worst = 0;
      for (UINT y = 20; y < kBackHeight; ++y)  // below the rectangle written above
        for (UINT x = 0; x < kBackWidth; ++x)
          worst = std::max(worst, rampDifference8(static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4,
                                                  x, y));
      sysmem->UnlockRect();
    }
    if (sysmem)
      sysmem->Release();
    char what[120];
    std::snprintf(what, sizeof what, "GetRenderTargetData of the back buffer into X8R8G8B8: largest difference %d/255",
                  worst);
    check(worst <= 1, what);
  }
  {
    // Full-screen A8R8G8B8 render targets become 16-bit float but report A8R8G8B8.
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* level = nullptr;
    D3DSURFACE_DESC levelDesc = {}, surfaceDesc = {};
    device->CreateTexture(kBackWidth, kBackHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture,
                          nullptr);
    if (texture) {
      texture->GetLevelDesc(0, &levelDesc);
      texture->GetSurfaceLevel(0, &level);
    }
    if (level)
      level->GetDesc(&surfaceDesc);
    check(levelDesc.Format == D3DFMT_A8R8G8B8 && surfaceDesc.Format == D3DFMT_A8R8G8B8,
          "a full-screen A8R8G8B8 render target texture reports A8R8G8B8 (GetLevelDesc, and GetDesc of its surface)");
    std::vector<float> copied;
    if (level && SUCCEEDED(device->StretchRect(backBuffer, nullptr, level, nullptr, D3DTEXF_NONE)))
      copied = readFp16(device, level);
    float rgb[3];
    rampColour(kBackWidth - 1, 30, rgb);
    check(!copied.empty() && std::fabs(copied[(30 * size_t(kBackWidth) + kBackWidth - 1) * 4] - rgb[0]) < kTolerance,
          "... and really is 16-bit float: it keeps the back buffer's values above white");
    if (level)
      level->Release();
    if (texture)
      texture->Release();

    IDirect3DSurface9* target = nullptr;
    D3DSURFACE_DESC targetDesc = {};
    device->CreateRenderTarget(kBackWidth, kBackHeight, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target,
                               nullptr);
    if (target)
      target->GetDesc(&targetDesc);
    check(targetDesc.Format == D3DFMT_A8R8G8B8 && target && !readFp16(device, target).empty(),
          "a full-screen A8R8G8B8 render target surface reports A8R8G8B8 and really is 16-bit float");
    if (target)
      target->Release();

    IDirect3DTexture9* own = nullptr;
    D3DSURFACE_DESC ownDesc = {};
    device->CreateTexture(kBackWidth, kBackHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
                          &own, nullptr);
    if (own) {
      own->GetLevelDesc(0, &ownDesc);
      own->Release();
    }
    check(ownDesc.Format == D3DFMT_A16B16G16R16F, "a render target the game makes 16-bit float itself reports that");
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
