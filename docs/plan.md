# NewVegasHDR: design

## Goal

Real HDR output for Fallout: New Vegas, with the game's own look: everything the game drew below white looks exactly
as it did in SDR. What the game's 8-bit pipeline clipped at white (sun, sky, fire, lamps, explosions, specular
highlights) comes out brighter than white, up to the display's peak. The HUD and menus sit at a set paper white.
Vanilla Plus Tonemapper's curve is a setting.

It is a standalone xNVSE plugin. It changes no game files and no other mod's files, and uses only DXVK's public D3D9
extension interfaces.

## Why it works this way

- **D3D9 has no HDR output, but DXVK does.** DXVK, which Viva New Vegas installs as the game's `d3d9.dll`, has two
  public extension interfaces for this (its `d3d9_interfaces.h`, zlib licence; `src/dxvk_ext.h` declares them).
  `ID3D9VkExtInterface::UnlockAdditionalFormats` allows an `A16B16G16R16F` back buffer.
  `ID3D9VkExtSwapchain::SetColorSpace` switches the Vulkan swap chain to scRGB (`VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`),
  and `SetHDRMetaData` passes the metadata.
- **The installed DXVK is enough on NVIDIA.** DXVK 3.0 to 3.1.1 never enable `VK_EXT_swapchain_colorspace` on their
  Vulkan instance, but NVIDIA's driver accepts the colour space anyway. `tests/dxvk_hdr_probe.cpp` got an
  `R16G16B16A16_SFLOAT` + `EXTENDED_SRGB_LINEAR` swap chain on DXVK 3.0.1. DXVK master has the spec-correct fix
  (3c40ca1b94), and `tools/fetch_dxvk.ps1` fetches a master CI build to test with. Other drivers may need that fix.
- **The game already renders in HDR; it just throws the highlights away.** The scene is lit into 16-bit float targets,
  but the HDR blend pass (`ISHDRBLENDINSHADER`) exposes it, adds bloom, grades it and writes the result to an 8-bit
  target. Everything after that (image space effects, HUD) is 8-bit, ending in an `X8R8G8B8` back buffer. Values above
  1.0 are clipped at the blend pass.
- **No import hooks.** The game imports only `D3DPERF_SetOptions` from `d3d9.dll` and gets `Direct3DCreate9` with
  `GetProcAddress`. So the plugin hooks DXVK directly, and patches the device's vtable when the game creates its
  device.
- **Device creation is hooked inside DXVK, not in the vtable.** On some launches Windows' compatibility shim engine
  (`apphelp.dll`) hooks `CreateDevice` in the vtable that every `IDirect3D9` shares. It always did on the first run of
  a new executable here. It hooks it again at every `Direct3DCreate9` and calls straight through to the function it
  found first, so a vtable hook installed in between is skipped. DXVK's `CreateDevice` calls its `CreateDeviceEx`
  directly, so every route ends there. The plugin puts a jump over the start of that function (`src/detour.cpp`).
  It moves only plain prologue instructions (DXVK 3.0.1's is `push ebp; mov ebp, esp; sub esp, 0x30`). If the
  function starts otherwise, the plugin falls back to the vtable slot and says so in the log.
- **The game must never see a 16-bit float format where it asked for an 8-bit one.** The first start in game
  (2026-10-22) crashed in Gamebryo's renderer setup, right after the device was created, while it built the render
  target group for the back buffer. Its pixel format lookup had no entry for `A16B16G16R16F`, and an `NiPixelData`
  copied its format from the null pointer it got (`rep movsd` from `esi = 0`, 0x11 dwords = one `NiPixelFormat`). So
  the plugin reports the game's own formats everywhere (`src/masquerade.cpp`, below).

## The game's tonemapper

Shader packages are `Data\Shaders\shaderpackage*.sdp`: a header (u32 version 100, u32 count, u32 size), then per
shader a 256-byte name, a u32 size and the D3D9 bytecode with its constant table. `tools/sdp.py` lists, extracts and
diffs them. On current GPUs the game uses package 017 (render path `BSSM_SV_2_B`, pixel shaders `ps_2_x`).

`ISHDRBLENDINSHADER` comes in three variants with these registers:

| | registers |
|---|---|
| all | `HDRParam` c1 (x: target luminance), `BlurScale` c2 (z: output alpha), `s0` bloom (rgb) + adapted luminance (a), `s1` scene |
| `CIN` | + `Cinematic` c19 (saturation, contrast pivot, contrast, brightness), `Tint` c20, `Fade` c22 |
| `CINAM` | + `UseAlphaMask` c23 (w: how much of the result unmasked pixels get) |

The game's math has no curve at all:

```
exposure = HDRParam.x / max(bloom.a, HDRParam.x)
color    = exposure * scene + max(bloom.rgb * 0.5 / max(bloom.a, HDRParam.x), 0)
CIN:     grey = dot(color, (0.299, 0.587, 0.114)); color = lerp(grey, color, Cinematic.x)
         color = lerp(color, grey * Tint.rgb, Tint.a)
         color = (Cinematic.w * color - Cinematic.y) * Cinematic.z + Cinematic.y
         color = lerp(color, Fade.rgb, Fade.a)
CINAM:   raw = scene at uv0; color = raw.a == 0 ? raw : lerp(raw, color, UseAlphaMask.w)
the 8-bit target clips color to [0, 1]
```

Vanilla Plus Tonemapper (VPT) ships `ps_3_0` versions as loose shaders (loaded by Shader Loader - NVSE) with their
own `vs_3_0`. The exposure and grading are the same, with Rec. 709 luma weights (0.2116, 0.7152, 0.0722). Before the
fade, VPT adds a curve: `max(color, 0)`, `pow 2.2` to linear, a slight contrast curve in log2 space
(`exp2((log2(x + 1e-4) - 0.18) * 1.02 + 0.18) - 1e-4`), extended Reinhard on luminance (`x * (1 + 0.16 L) / (1 + L)`,
white at 2.5), and `pow 1/2.2` back. The plugin recognises VPT's shaders by its float constants 0.16 and 1.02.

## The plugin's pipeline

1. **Load** (`NVSEPlugin_Load`): call DXVK's `Direct3DCreate9` and check for `ID3D9VkExtInterface`. Without DXVK the
   plugin does nothing more. With it, the plugin hooks DXVK's `CreateDeviceEx` in place, which `CreateDevice` also
   goes through.
2. **CreateDevice.** Find the window's display through DXGI (`IDXGIOutput6`: Windows HDR on or off, peak nits). HDR
   goes on when it is enabled in the ini, d3d9 is DXVK, and Windows HDR is on (or `ForceHdr`). Then:
   - `UnlockAdditionalFormats` on the game's own `IDirect3D9`: the unlock is per interface and copied into the device
     it creates.
   - The back buffer becomes `A16B16G16R16F`. The game still sees the format it asked for: its present parameters
     get it back after the call, and everything else goes through step 6.
   - The swap chain switches to scRGB, with metadata: BT.709 primaries, D65, max luminance and MaxCLL = peak, MaxFALL
     = paper white.
   - The device's vtable gets hooked.

   If any step fails, the plugin falls back to the game's own format and the game stays SDR.
3. **CreatePixelShader.** The three HDR blend variants are recognised by their constant tables, whether they come from
   the game's package or from VPT, and swapped for ours (`src/shaders/hdr_blend.hlsl`). Ours are built for the same
   shader model as the original, so they pair with its vertex shader: `ps_2_b` for the game's `vs_1_1`, `ps_3_0` for
   VPT's `vs_3_0`.
4. **Our HDR blend** does the same math in the same order: exposure, bloom, grading, VPT's curve when the look is
   VanillaPlus, fade and alpha mask. Only the end differs:
   - SDR (`Tonemap.z = 0`): `saturate`, the same as the 8-bit target. This is exact.
   - HDR: the value is taken to linear light with the decode gamma. A shoulder leaves everything up to
     `ShoulderStart x peak` alone and rolls the rest off exponentially towards the peak. It works on the brightest
     channel, so hues hold, and it is continuous in value and slope. The value then goes back to gamma.

   VPT's curve squeezes nearly everything under white. In the first in-game capture (Goodsprings, daylight) only
   0.04 % of the pixels went above it: the saloon's light bulbs. The sky averaged 109 nits, and HDR looked the same as
   SDR. So in HDR the curve can be stretched: its Reinhard runs on luminance / k, with
   `k = 1 + HighlightRange x (peak / paper white - 1)`. With `HighlightRange=1` (the default) skies, sunlit ground
   and lights use the display's range, and shadows barely move (within 1.5 % below a quarter of white). Replayed on
   that capture, the sky averages 158 nits and the frame 81 instead of 57. `HighlightRange=0` is the SDR curve. The
   game's own look has no curve, so it already uses the full range: 183 nits for that sky.

   The second capture (frame 15362, daylight, the stretched curve in game) matched its replay exactly: sky 160 nits,
   frame 79. The SDR curve gives 109 and 55 there, the game's own look 186 and 91. Compared live, the stretched curve
   was clearly brighter, and we chose the game's own look. In HDR nothing is cut off at white, so a curve that
   keeps highlights under white has no job left: the game's own image is the faithful one, and VPT's is a style. So
   `ToneMapper` defaults to `Vanilla`, and it applies to HDR output only. SDR output (Windows HDR off) and `SdrExact`
   keep the curve of the shaders the game loaded, so that they still look as the game does without the plugin.

   The output stays in the game's gamma space, so every later pass (image space effects, HUD) works on what it
   always did. It only goes above 1 where the old target clipped, or where the stretched curve reaches.

   The output alpha (`BlurScale.z`) is what the game's target would keep of it. The game sometimes leaves that
   constant unset (NaN in the capture):
   - the back buffer the game made `X8R8G8B8` gets 1;
   - an 8-bit target the plugin made 16-bit float gets the value clipped to [0, 1];
   - a float target of the game's own gets the value as it is. The plugin's constants go in c24 (peak / paper white,
   look, HDR on, shoulder start) and c25 (gamma, 1 / gamma). They are set before each draw of the shader and put back
   after it, in case the engine skips setting constants it thinks are already there.
5. **Render targets.** Full-screen `A8R8G8B8` render targets (exactly the back buffer's size, not shared, not lockable)
   are created as `A16B16G16R16F`, so values above 1 survive the passes between the blend and the back buffer.
6. **What the game sees** (`src/masquerade.cpp`). Every surface and texture whose format the plugin changed carries
   the format the game asked for as private data: the back buffers (again after each `Reset`), the upgraded textures
   and each of their levels, and the upgraded render target surfaces. Hooks on the vtables that all surfaces, all
   textures and all swap chains share report that format:
   - `IDirect3DSurface9::GetDesc`, `IDirect3DTexture9::GetLevelDesc` and `IDirect3DSwapChain9::GetPresentParameters`;
   - `IDirect3DSurface9::LockRect` on such a surface converts it into an 8-bit render target of the game's format,
     reads that back to system memory and hands the game the copy. `UnlockRect` writes back only the locked rectangle
     (unless the lock was read-only), so highlights elsewhere stay;
   - DXVK copies to and from system memory (`GetRenderTargetData`, `UpdateSurface`) only between equal formats. When
     it refuses one because of a changed format, the device hook makes the copy through an 8-bit render target.

   The plugin's own calls (`InternalScope`) and its log and traces see the real formats. The log names the caller
   (module + offset) of the first few calls that saw a changed format.
7. **Present.** The plugin copies the back buffer (resolving it if it is multisampled), then draws the copy back
   through `src/shaders/encode.hlsl`: `scRGB = pow(value, Gamma) x PaperWhiteNits / 80`, capped at `peak / 80`. White
   UI lands at paper white. After the present it draws the copy back unchanged, so the game never finds an scRGB
   frame in its back buffer: one drawn over without a clear would otherwise be decoded twice. The pass saves and
   restores all device state it touches with a state block.
8. **Gamma ramps** are ignored while HDR is on. DXVK applies them in fullscreen through a lookup table that clips at 1.

`Reset` repeats the back buffer, tagging and colour space steps. The frame copy, the state block and the conversion
surfaces are released before it.

## Settings

`NewVegasHDR.ini` next to the DLL (the default is `package/NewVegasHDR.ini`). It is re-read every 60 frames when its
modification time changes. `Enabled`, `ReplaceShaders` and `UpgradeRenderTargets` need a restart.

| key | default | meaning |
|---|---|---|
| `[HDR] Enabled` | 1 | HDR output when possible |
| `PeakNits` | 0 | display peak; 0 = what Windows reports (1000 if it reports nothing usable) |
| `PaperWhiteNits` | 203 | brightness of SDR white, HUD and menus |
| `ToneMapper` | Vanilla | the look of HDR output: `Vanilla` (the game's own), `VanillaPlus`, or `Auto` (the loaded shaders'); SDR output keeps the loaded shaders' look |
| `HighlightRange` | 1 | how much of the display's range VPT's curve may use in HDR: 0 = as in SDR, 1 = up to the peak |
| `ShoulderStart` | 0.5 | start of the highlight roll-off, as a fraction of the peak |
| `Gamma` | 2.2 | decode gamma of the game's image |
| `[Advanced] ReplaceShaders` | 1 | swap in our HDR blend shaders |
| `UpgradeRenderTargets` | 1 | full-screen 8-bit targets as 16-bit float |
| `SdrExact` | 0 | our shaders output exactly the SDR image (for A/B comparisons) |
| `ForceHdr` | 0 | HDR even when Windows says HDR is off |

## Verification

- `test.ps1` builds and runs `tests/harness.cpp` on a copy of the game's DXVK, in an off-screen window, without the
  game. It creates its device as the game does (two lockable `X8R8G8B8` back buffers, a multithreaded pure device),
  and renders a synthetic HDR scene through the original shaders (vanilla and VPT, all three variants) and through
  the plugin's. The checks:
  - With `SdrExact=1` the plugin's shaders match exactly.
  - With HDR on and `HighlightRange=0` they match wherever the original stayed below white, go above white where it
    clipped, and stay at or below the peak.
  - With `HighlightRange=1` VPT's look gets brighter highlights, no pixel gets darker by its brightest channel, the
    shadows stay within 3 %, and the peak still holds. The game's own look does not change.
  - With `SdrExact=1` and `ToneMapper=Vanilla`, VPT's shaders still give VPT's image. In HDR without a `ToneMapper`
    key (the default) they give the game's own look.
  - The output alpha is 1 on the back buffer, clipped on an upgraded 8-bit target (NaN gives 0), and unchanged on a
    float target of the game's own.
  - The plugin puts the game's c24/c25 back after each draw.
  - Device creation is still caught when a stand-in for Windows' shim hooks `CreateDevice` over the plugin.
  - The presented scRGB frame is the decoded gamma frame, scaled to paper white and capped at the peak.
  - After the present the back buffer holds the game's frame again.
  - Both back buffers and the swap chain report `X8R8G8B8`, and the upgraded render targets (textures and surfaces)
    `A8R8G8B8`, while really being 16-bit float. A render target the game makes 16-bit float itself still reports so.
  - A read-only lock of the back buffer gets the frame as `X8R8G8B8`. A lock that writes changes only its rectangle.
    `GetRenderTargetData` of the back buffer into an `X8R8G8B8` surface works.

  It runs at gamma 2.4, so the shaders and the decode must both follow the ini.
- `tests/dxvk_hdr_probe.cpp`: does a given `d3d9.dll` give an HDR swap chain on this machine.
- `tests/vtable_check.py`: the vtable slot numbers in `src/vtable.h` against the Windows SDK's `d3d9.h`.
- In game: `NewVegasHDR.log` in the game folder lists the device, display, config, every HDR blend shader seen and
  replaced, render target creation and upgrades, and each kind of surface copy (save thumbnails, menu backgrounds).
  Creating `NewVegasHDR.trace` in the game folder traces the next frame. Its first line, if any, is the output
  folder. The plugin deletes the file and writes:
  - `trace-<frame>.txt`: render target changes, clears, copies and draws, with the HDR blend constants;
  - `.dds` dumps of the HDR blend's bloom, scene and output, and of the finished frame before and after the decode.

## Open questions and next steps

- How bright the game's highlights really are in interiors and at night (daylight is above, under step 4): capture
  traces and tune `PaperWhiteNits`, `HighlightRange`, `ShoulderStart` and the peak. On OLEDs, large bright areas
  such as the desert sky trigger the panel's brightness limiter, so the defaults should keep big areas modest. The
  daylight captures averaged 55 to 57 nits with VPT's SDR curve, 79 to 81 with it stretched and 91 with the game's
  own look, well under this panel's 277 nits full-frame limit.
- What reads the upgraded targets or the back buffer back to the CPU (save game thumbnails, screenshots): the `copy`
  lines and the lines naming a caller in the log show it. Check that saves still get thumbnails.
- The back buffer now has an alpha channel that holds what the game's draws wrote, where `X8R8G8B8` read as 1. A draw
  that blends with the destination's alpha would see the difference. Watch the HUD and menus for it.
- UI brightness separate from paper white: the HUD is drawn into the same back buffer, so this needs the HUD pass
  told apart from the scene.
- Cost: one extra full-screen copy and two full-screen passes per frame at 16-bit float.
