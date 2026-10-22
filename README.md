# NewVegasHDR

An [xNVSE](https://github.com/xNVSE/NVSE) plugin that gives Fallout: New Vegas real HDR output through
[DXVK](https://github.com/doitsujin/dxvk). Everything the game drew below white looks as it did in SDR, with the game's
own look or [Vanilla Plus Tonemapper](https://www.nexusmods.com/newvegas/mods/91500)'s. What the game's 8-bit pipeline
clipped at white (sun, sky, fire, lamps, highlights) comes out brighter than white, up to your display's peak, and the
HUD sits at a paper white you choose.

This is a personal project, developed on one machine: RTX 5070 Ti, 4K HDR OLED, the Viva New Vegas modlist, xNVSE
6.4.8, DXVK 3.0.1. **Status: in development. It passes its test harness; in-game testing has just started.**

## How it works

The game already renders its scene in 16-bit float. Its HDR blend pass then exposes the scene, adds bloom and grades
it, and writes the result to an 8-bit target that clips everything above 1. The plugin:

1. Creates the game's swap chain with a 16-bit float back buffer and switches it to scRGB through DXVK's D3D9
   extension interfaces.
2. Swaps the game's (or Vanilla Plus Tonemapper's) HDR blend shaders for its own. They do the same math, but keep
   what lies above white and roll it off smoothly towards the display's peak.
3. Creates the full-screen 8-bit render targets after the blend as 16-bit float, so highlights survive to the end of
   the frame.
4. Decodes the finished frame, HUD included, from the game's gamma space to scRGB just before it is presented.
5. Shows the game the 8-bit formats it asked for wherever it can read a format, and converts locks and copies to
   system memory to match, because the engine cannot handle a 16-bit float back buffer.

[docs/plan.md](docs/plan.md) has the details: the game's tonemapper, the pipeline, and how it is tested.

## Requirements

- Fallout: New Vegas with xNVSE.
- DXVK as the game's `d3d9.dll`; Viva New Vegas sets this up. DXVK 3.0.1 works on NVIDIA. DXVK releases up to 3.1.1
  rely on the driver accepting the HDR colour space without the Vulkan extension that announces it; NVIDIA's does.
- Windows HDR on for the display. Without it, or without DXVK, the plugin leaves the image exactly as it was.

## Install (Mod Organizer 2)

Make a mod with `NVSE\Plugins\NewVegasHDR.dll` and `NVSE\Plugins\NewVegasHDR.ini`, and enable it.
`install.ps1 -Mo2 <MO2 folder>` does this from a build (close Mod Organizer and the game first). To undo, untick the
mod. The plugin writes `NewVegasHDR.log` in the game folder.

## Settings

`NewVegasHDR.ini` (defaults in [package/NewVegasHDR.ini](package/NewVegasHDR.ini)). It is re-read while the game runs:

- `PeakNits`: the display's peak; 0 uses what Windows reports.
- `PaperWhiteNits`: how bright white is (HUD, menus, what was white in SDR), 203 by default.
- `HighlightRange`: how much of the display's range Vanilla Plus Tonemapper's curve may use. 1 (the default) lets
  skies, sunlit ground and lights get brighter; 0 keeps its SDR curve, so only what clipped at white gets brighter.
- `ShoulderStart`: where highlights start rolling off, as a fraction of the peak.
- `ToneMapper`: `Auto` keeps whatever look the game loaded; `Vanilla` and `VanillaPlus` force one.
- `Gamma`: how the game's image is decoded, 2.2 by default.
- `SdrExact=1`: reproduces the SDR image exactly, for comparisons.

## Building and testing

Visual Studio 2022 or newer with the C++ x86 tools, CMake and Ninja (both come with Visual Studio), and the Windows
SDK (for `fxc`).

```powershell
.\build.ps1   # build\NewVegasHDR.dll, build\harness.exe, build\dxvk_hdr_probe.exe
.\test.ps1    # builds, then checks the plugin against the game's own shaders on DXVK without starting the game
```

`test.ps1` reads DXVK and the vanilla shaders from the game folder. It needs Vanilla Plus Tonemapper's
`ISHDRBLENDINSHADER*.pso`/`.vso` copied into `extracted\vpt`.

## License

MIT (see [LICENSE](LICENSE)). The DXVK interface declarations in `src/dxvk_ext.h` follow DXVK's public
`d3d9_interfaces.h` (zlib licence).
