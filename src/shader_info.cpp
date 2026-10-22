#include "shader_info.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr DWORD kEnd = 0x0000FFFF;
constexpr DWORD kCommentOpcode = 0xFFFE;
constexpr DWORD kCtab = 0x42415443;  // 'CTAB'

// Instruction lengths are encoded in bits 24-27 from shader model 2 on. Shader model 1 has none: there the parameter
// tokens after an instruction have bit 31 set, except the literals of def, defi and defb.
size_t bytecodeSize(const DWORD* code) {
  const DWORD* p = code + 1;
  const bool sm2 = ((code[0] >> 8) & 0xFF) >= 2;
  for (int guard = 0; guard < 1 << 16; ++guard) {
    DWORD token = *p;
    if (token == kEnd)
      return (p + 1 - code) * sizeof(DWORD);
    DWORD opcode = token & 0xFFFF;
    if (opcode == kCommentOpcode) {
      p += 1 + ((token >> 16) & 0x7FFF);
    } else if (sm2) {
      p += 1 + ((token >> 24) & 0x0F);
    } else if (opcode == 0x51 || opcode == 0x30) {  // def, defi: register + 4 literals
      p += 6;
    } else if (opcode == 0x2F) {  // defb: register + 1 literal
      p += 3;
    } else {
      ++p;
      while (*p & 0x80000000)
        ++p;
    }
  }
  return 0;
}

void readConstantTable(const DWORD* code, ShaderInfo& info) {
  const DWORD* p = code + 1;
  while ((*p & 0xFFFF) == kCommentOpcode) {
    DWORD length = (*p >> 16) & 0x7FFF;
    if (length >= 8 && p[1] == kCtab) {
      const char* base = reinterpret_cast<const char*>(p + 2);
      const size_t bytes = (length - 1) * sizeof(DWORD);
      DWORD count = reinterpret_cast<const DWORD*>(base)[3];
      DWORD infoOffset = reinterpret_cast<const DWORD*>(base)[4];
      if (infoOffset + count * 20 > bytes)
        return;
      for (DWORD i = 0; i < count; ++i) {
        const char* entry = base + infoOffset + i * 20;
        DWORD nameOffset;
        WORD set, index, regCount;
        std::memcpy(&nameOffset, entry, 4);
        std::memcpy(&set, entry + 4, 2);
        std::memcpy(&index, entry + 6, 2);
        std::memcpy(&regCount, entry + 8, 2);
        if (nameOffset >= bytes)
          continue;
        ShaderConstant c;
        c.name.assign(base + nameOffset, strnlen(base + nameOffset, bytes - nameOffset));
        c.set = "bics"[set & 3];
        c.index = index;
        c.count = regCount;
        info.constants.push_back(std::move(c));
      }
      return;
    }
    p += 1 + length;
  }
}

}  // namespace

const ShaderConstant* ShaderInfo::find(const char* name) const {
  for (const auto& c : constants)
    if (c.name == name)
      return &c;
  return nullptr;
}

bool ShaderInfo::has(char set, int index) const {
  for (const auto& c : constants)
    if (c.set == set && index >= c.index && index < c.index + c.count)
      return true;
  return false;
}

std::string ShaderInfo::summary() const {
  char head[96];
  std::snprintf(head, sizeof head, "%s_%d_%s %zu B %016llx:", pixel ? "ps" : "vs", major,
                minor == 1 && major == 2 ? "x" : (minor ? "1" : "0"), size, static_cast<unsigned long long>(hash));
  std::string s = head;
  for (const auto& c : constants) {
    char buf[96];
    std::snprintf(buf, sizeof buf, " %s@%c%d", c.name.c_str(), c.set, c.index);
    s += buf;
  }
  return s;
}

ShaderInfo parseShader(const DWORD* code) {
  ShaderInfo info;
  if (!code)
    return info;
  info.pixel = (code[0] >> 16) == 0xFFFF;
  info.major = (code[0] >> 8) & 0xFF;
  info.minor = code[0] & 0xFF;
  info.size = bytecodeSize(code);
  uint64_t h = 0xcbf29ce484222325ull;
  const auto* bytes = reinterpret_cast<const unsigned char*>(code);
  for (size_t i = 0; i < info.size; ++i)
    h = (h ^ bytes[i]) * 0x100000001b3ull;
  info.hash = h;
  readConstantTable(code, info);
  return info;
}

HdrBlend classifyHdrBlend(const ShaderInfo& info) {
  if (!info.pixel)
    return HdrBlend::None;
  const ShaderConstant* hdr = info.find("HDRParam");
  const ShaderConstant* blur = info.find("BlurScale");
  if (!hdr || hdr->set != 'c' || hdr->index != 1 || !blur || blur->set != 'c' || blur->index != 2)
    return HdrBlend::None;
  if (!info.has('s', 0) || !info.has('s', 1) || info.find("TimingData") || info.find("BlurOffsets") ||
      info.find("AvgLum"))
    return HdrBlend::None;
  const ShaderConstant* cinematic = info.find("Cinematic");
  if (!cinematic)
    return HdrBlend::Plain;
  if (cinematic->index != 19 || !info.has('c', 20) || !info.has('c', 22))
    return HdrBlend::None;
  const ShaderConstant* mask = info.find("UseAlphaMask");
  if (!mask)
    return HdrBlend::Cinematic;
  return mask->index == 23 ? HdrBlend::CinematicAlphaMask : HdrBlend::None;
}

const char* hdrBlendName(HdrBlend kind) {
  switch (kind) {
    case HdrBlend::Plain: return "ISHDRBLENDINSHADER";
    case HdrBlend::Cinematic: return "ISHDRBLENDINSHADERCIN";
    case HdrBlend::CinematicAlphaMask: return "ISHDRBLENDINSHADERCINAM";
    default: return "none";
  }
}

bool hasVanillaPlusCurve(const DWORD* code, size_t size) {
  bool white = false, contrast = false;
  for (size_t i = 0; i < size / sizeof(DWORD); ++i) {
    white |= code[i] == 0x3E23D70A;  // 0.16f
    contrast |= code[i] == 0x3F828F5C;  // 1.02f
  }
  return white && contrast;
}
