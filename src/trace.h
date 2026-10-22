// Frame trace, for working out the game's render passes without a debugger. When the file NewVegasHDR.trace appears
// in the game folder, the next frame's render target changes, copies, clears and draws are written to
// trace-<frame>.txt, and the inputs and output of the HDR blend pass and the finished frame (before and after the
// scRGB decode) are saved as .dds files. They go to the folder named on the trigger file's first line, or to
// NewVegasHDR-captures in the game folder.
#pragma once

#include <windows.h>
#include <d3d9.h>

#include <string>

struct HdrBlendShader;

namespace trace {

void poll();  // at every Present: looks for the trigger file every 30 frames
bool active();
void beginFrame();  // after a Present: starts tracing if armed
void beforeEncode(IDirect3DDevice9* device);  // at Present
void afterEncode(IDirect3DDevice9* device);  // at Present, ends the trace

void line(const char* format, ...);
void draw(IDirect3DDevice9* device, const char* call, UINT primitives, const HdrBlendShader* hdrBlend);
void afterDraw(IDirect3DDevice9* device, const HdrBlendShader* hdrBlend);

std::string describe(IDirect3DSurface9* surface);
std::string describe(IDirect3DBaseTexture9* texture);
// What is bound now, asked from the device (the hooks' own copies can be stale after a state block).
std::string describeTarget(IDirect3DDevice9* device);
std::string describeStage(IDirect3DDevice9* device, DWORD stage);

}  // namespace trace
