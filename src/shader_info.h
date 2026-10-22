// Reads what a D3D9 shader's bytecode says about itself: its model and the constant table the HLSL compiler leaves in
// a comment block (names and registers), and recognises the game's HDR blend shaders by it.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct ShaderConstant {
  std::string name;
  char set;  // 'b', 'i', 'c' or 's'
  int index;
  int count;
};

struct ShaderInfo {
  bool pixel = false;
  int major = 0, minor = 0;
  size_t size = 0;  // bytes
  uint64_t hash = 0;  // FNV-1a of the bytecode
  std::vector<ShaderConstant> constants;

  const ShaderConstant* find(const char* name) const;
  bool has(char set, int index) const;
  std::string summary() const;  // "ps_2_x 512 B 0123456789abcdef: HDRParam@c1 BlurScale@c2 ..."
};

ShaderInfo parseShader(const DWORD* code);

// The game's ISHDRBLENDINSHADER, ...CIN and ...CINAM, vanilla or replaced by a mod: HDRParam in c1, BlurScale in c2,
// the bloom in s0 and the scene in s1, and nothing of the adaptation or blur passes that share those constants.
enum class HdrBlend { None, Plain, Cinematic, CinematicAlphaMask };
HdrBlend classifyHdrBlend(const ShaderInfo& info);
const char* hdrBlendName(HdrBlend kind);

// True if the bytecode holds Vanilla Plus Tonemapper's curve constants (0.16 and 1.02 as float literals).
bool hasVanillaPlusCurve(const DWORD* code, size_t size);
