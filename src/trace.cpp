#include "trace.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_set>

#include "dds.h"
#include "log.h"
#include "state.h"

namespace trace {
namespace {

bool g_armed = false;
bool g_active = false;
FILE* g_file = nullptr;
std::wstring g_dir;
uint64_t g_frame = 0;
int g_hdrBlendDraws = 0;
int g_dumps = 0;
std::unordered_set<const void*> g_described;  // shaders whose summary this trace already has

// Consecutive draws with the same target and shaders are written as one run.
struct Run {
  IDirect3DSurface9* target = nullptr;
  const void* pixelShader = nullptr;
  const void* vertexShader = nullptr;
  const char* call = nullptr;
  int draws = 0;
  UINT primitives = 0;
} g_run;

void writeLine(const char* text) {
  if (g_file) {
    std::fputs(text, g_file);
    std::fputc('\n', g_file);
  }
}

void flushRun() {
  if (!g_run.draws)
    return;
  char buf[80];
  std::snprintf(buf, sizeof buf, "    %d draw%s, %u primitives", g_run.draws, g_run.draws == 1 ? "" : "s",
                g_run.primitives);
  writeLine(buf);
  g_run = {};
}

std::string shaderLabel(const void* shader) {
  if (!shader)
    return "none";
  char buf[24];
  std::snprintf(buf, sizeof buf, "%p", shader);
  std::string s = buf;
  if (g_described.insert(shader).second) {
    std::lock_guard lock(g.mutex);
    auto it = g.shaders.find(shader);
    if (it != g.shaders.end())
      s += " (" + it->second.summary() + ")";
  }
  return s;
}

std::wstring capturePath(const char* name) {
  wchar_t buf[96];
  std::swprintf(buf, 96, L"\\f%llu-%hs", static_cast<unsigned long long>(g_frame), name);
  return g_dir + buf;
}

// Copies a render target surface to system memory and writes it out. Textures that are not render targets cannot be
// read back this way and are skipped.
bool dumpSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const char* name) {
  if (!surface)
    return false;
  InternalScope internal;
  D3DSURFACE_DESC desc;
  if (FAILED(surface->GetDesc(&desc)) || !ddsBytesPerPixel(desc.Format) || desc.Pool != D3DPOOL_DEFAULT ||
      !(desc.Usage & D3DUSAGE_RENDERTARGET))
    return false;
  IDirect3DSurface9* source = surface;
  IDirect3DSurface9* resolved = nullptr;
  if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
    if (FAILED(g.d3d.createRenderTarget(device, desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                        &resolved, nullptr)))
      return false;
    g.d3d.stretchRect(device, surface, nullptr, resolved, nullptr, D3DTEXF_NONE);
    source = resolved;
  }
  bool ok = false;
  IDirect3DSurface9* sysmem = nullptr;
  if (SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sysmem,
                                                    nullptr)) &&
      SUCCEEDED(g.d3d.getRenderTargetData(device, source, sysmem))) {
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      ok = writeDds(capturePath(name), desc.Width, desc.Height, desc.Format, locked.pBits, locked.Pitch);
      sysmem->UnlockRect();
    }
  }
  if (sysmem)
    sysmem->Release();
  if (resolved)
    resolved->Release();
  if (ok)
    ++g_dumps;
  return ok;
}

bool dumpStage(IDirect3DDevice9* device, DWORD stage, const char* name) {
  IDirect3DBaseTexture9* texture = nullptr;
  device->GetTexture(stage, &texture);
  if (!texture)
    return false;
  bool ok = false;
  IDirect3DSurface9* surface = nullptr;
  if (texture->GetType() == D3DRTYPE_TEXTURE &&
      SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetSurfaceLevel(0, &surface))) {
    ok = dumpSurface(device, surface, name);
    surface->Release();
  }
  texture->Release();
  return ok;
}

bool dumpTarget(IDirect3DDevice9* device, const char* name) {
  IDirect3DSurface9* target = nullptr;
  device->GetRenderTarget(0, &target);
  if (!target)
    return false;
  bool ok = dumpSurface(device, target, name);
  target->Release();
  return ok;
}

void dumpBackBuffer(IDirect3DDevice9* device, const char* name) {
  IDirect3DSurface9* backBuffer = nullptr;
  if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
    if (!dumpSurface(device, backBuffer, name))
      line("  (could not save %s)", name);
    backBuffer->Release();
  }
}

}  // namespace

void poll() {
  if (g_armed || g_active || g.frame % 30)
    return;
  const std::wstring trigger = g.gameDir + L"NewVegasHDR.trace";
  if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES)
    return;
  char text[1024] = {};
  if (FILE* f = _wfopen(trigger.c_str(), L"rb")) {
    std::fread(text, 1, sizeof text - 1, f);
    std::fclose(f);
  }
  DeleteFileW(trigger.c_str());
  std::string first(text, strcspn(text, "\r\n"));
  wchar_t wide[1024] = {};
  MultiByteToWideChar(CP_UTF8, 0, first.c_str(), -1, wide, 1024);
  g_dir = *wide ? wide : g.gameDir + L"NewVegasHDR-captures";
  std::error_code error;
  std::filesystem::create_directories(g_dir, error);
  g_armed = true;
  LOG("trace: armed, output in %ls", g_dir.c_str());
}

bool active() { return g_active; }

void beginFrame() {
  if (!g_armed)
    return;
  g_armed = false;
  g_frame = g.frame + 1;
  wchar_t name[64];
  std::swprintf(name, 64, L"\\trace-%llu.txt", static_cast<unsigned long long>(g_frame));
  g_file = _wfopen((g_dir + name).c_str(), L"w");
  if (!g_file) {
    LOG("trace: cannot write to %ls", g_dir.c_str());
    return;
  }
  g_active = true;
  g_hdrBlendDraws = 0;
  g_dumps = 0;
  g_described.clear();
  g_run = {};
  line("frame %llu, back buffer %ux%u, game format %s, FP16 %d, scRGB %d, peak %.0f nits, paper white %.0f nits",
       static_cast<unsigned long long>(g_frame), g.width, g.height, formatName(g.gameFormat), g.fp16BackBuffer,
       g.scrgb, g.peakNits(), g.config.paperWhiteNits);
}

void beforeEncode(IDirect3DDevice9* device) {
  if (!g_active)
    return;
  line("Present");
  dumpBackBuffer(device, "frame-gamma.dds");
}

void afterEncode(IDirect3DDevice9* device) {
  if (!g_active)
    return;
  if (g.scrgb)
    dumpBackBuffer(device, "frame-scrgb.dds");
  flushRun();
  std::fclose(g_file);
  g_file = nullptr;
  g_active = false;
  LOG("trace: frame %llu written, %d HDR blend draws, %d images", static_cast<unsigned long long>(g_frame),
      g_hdrBlendDraws, g_dumps);
}

void line(const char* format, ...) {
  if (!g_file)
    return;
  flushRun();
  char buf[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof buf, format, args);
  va_end(args);
  writeLine(buf);
}

void draw(IDirect3DDevice9* device, const char* call, UINT primitives, const HdrBlendShader* hdrBlend) {
  if (!g_active || g.internal)
    return;
  if (g_run.draws && g_run.target == g.renderTarget0 && g_run.pixelShader == g.pixelShader &&
      g_run.vertexShader == g.vertexShader && g_run.call == call) {
    ++g_run.draws;
    g_run.primitives += primitives;
    return;
  }
  flushRun();
  std::string head = std::string("  ") + call + " ps " + shaderLabel(g.pixelShader) + ", vs " +
                     shaderLabel(g.vertexShader);
  writeLine(head.c_str());
  for (DWORD stage = 0; stage < 4; ++stage) {
    std::string s = describeStage(device, stage);
    if (s != "none") {
      s = "    s" + std::to_string(stage) + " " + s;
      writeLine(s.c_str());
    }
  }
  g_run = {g.renderTarget0, g.pixelShader, g.vertexShader, call, 1, primitives};

  if (hdrBlend) {
    ++g_hdrBlendDraws;
    line("    HDR blend %s (%s, %s): c1 %g %g %g %g, c2 %g %g %g %g, c19 %g %g %g %g, c20 %g %g %g %g, "
         "c22 %g %g %g %g, c23 %g %g %g %g",
         hdrBlendName(hdrBlend->kind), hdrBlend->replaced ? "ours" : "game's",
         hdrBlend->vanillaPlus ? "Vanilla Plus curve" : "vanilla curve", g.psConstants[1][0], g.psConstants[1][1],
         g.psConstants[1][2], g.psConstants[1][3], g.psConstants[2][0], g.psConstants[2][1], g.psConstants[2][2],
         g.psConstants[2][3], g.psConstants[19][0], g.psConstants[19][1], g.psConstants[19][2], g.psConstants[19][3],
         g.psConstants[20][0], g.psConstants[20][1], g.psConstants[20][2], g.psConstants[20][3], g.psConstants[22][0],
         g.psConstants[22][1], g.psConstants[22][2], g.psConstants[22][3], g.psConstants[23][0], g.psConstants[23][1],
         g.psConstants[23][2], g.psConstants[23][3]);
    if (g_hdrBlendDraws <= 2) {
      char name[48];
      std::snprintf(name, sizeof name, "hdrblend%d-bloom.dds", g_hdrBlendDraws);
      dumpStage(device, 0, name);
      std::snprintf(name, sizeof name, "hdrblend%d-scene.dds", g_hdrBlendDraws);
      dumpStage(device, 1, name);
    }
  }
}

void afterDraw(IDirect3DDevice9* device, const HdrBlendShader* hdrBlend) {
  if (!g_active || g.internal || !hdrBlend || g_hdrBlendDraws > 2)
    return;
  char name[48];
  std::snprintf(name, sizeof name, "hdrblend%d-out.dds", g_hdrBlendDraws);
  dumpTarget(device, name);
}

std::string describeTarget(IDirect3DDevice9* device) {
  IDirect3DSurface9* target = nullptr;
  device->GetRenderTarget(0, &target);
  std::string s = describe(target);
  if (target) {
    if (target == g.backBuffer)
      s += " (back buffer)";
    target->Release();
  }
  return s;
}

std::string describeStage(IDirect3DDevice9* device, DWORD stage) {
  IDirect3DBaseTexture9* texture = nullptr;
  device->GetTexture(stage, &texture);
  std::string s = describe(texture);
  if (texture)
    texture->Release();
  return s;
}

std::string describe(IDirect3DSurface9* surface) {
  if (!surface)
    return "none";
  D3DSURFACE_DESC desc;
  if (FAILED(surface->GetDesc(&desc)))
    return "?";
  char owner[40] = "";
  IDirect3DTexture9* texture = nullptr;
  if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&texture)))) {
    std::snprintf(owner, sizeof owner, " of texture %p", static_cast<void*>(texture));
    texture->Release();
  }
  char samples[16] = "";
  if (desc.MultiSampleType)
    std::snprintf(samples, sizeof samples, " msaa%d", static_cast<int>(desc.MultiSampleType));
  char buf[192];
  std::snprintf(buf, sizeof buf, "surface %p%s %ux%u %s%s%s", static_cast<void*>(surface), owner, desc.Width,
                desc.Height, formatName(desc.Format), samples,
                desc.Usage & D3DUSAGE_RENDERTARGET ? " rt" : (desc.Usage & D3DUSAGE_DEPTHSTENCIL ? " ds" : ""));
  return buf;
}

std::string describe(IDirect3DBaseTexture9* texture) {
  if (!texture)
    return "none";
  char buf[160];
  D3DSURFACE_DESC desc = {};
  const D3DRESOURCETYPE type = texture->GetType();
  if (type == D3DRTYPE_TEXTURE)
    static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc);
  else if (type == D3DRTYPE_CUBETEXTURE)
    static_cast<IDirect3DCubeTexture9*>(texture)->GetLevelDesc(0, &desc);
  std::snprintf(buf, sizeof buf, "%s %p %ux%u %s levels %lu%s", type == D3DRTYPE_CUBETEXTURE ? "cube" : "texture",
                static_cast<void*>(texture), desc.Width, desc.Height, formatName(desc.Format),
                texture->GetLevelCount(),
                desc.Usage & D3DUSAGE_RENDERTARGET ? " rt" : (desc.Usage & D3DUSAGE_DEPTHSTENCIL ? " ds" : ""));
  return buf;
}

}  // namespace trace
