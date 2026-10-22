#include "detour.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace detour {
namespace {

constexpr size_t kJumpSize = 5;  // E9 rel32

// Length of a ModRM operand: the ModRM byte, a SIB byte and the displacement.
size_t modrmLength(const uint8_t* p) {
  const uint8_t mod = p[0] >> 6, rm = p[0] & 7;
  size_t n = 1;
  if (mod != 3 && rm == 4) {
    ++n;
    if (mod == 0 && (p[1] & 7) == 5)
      n += 4;
  }
  if (mod == 1)
    n += 1;
  else if (mod == 2 || (mod == 0 && rm == 5))  // the latter an absolute address, which stays valid when moved
    n += 4;
  return n;
}

// Length of an instruction that can run anywhere unchanged, or 0 for anything else.
size_t movableLength(const uint8_t* p) {
  const uint8_t op = p[0];
  if (op >= 0x50 && op <= 0x5F)  // push, pop r32
    return 1;
  switch (op) {
    case 0x01: case 0x03: case 0x29: case 0x2B: case 0x31: case 0x33: case 0x85:  // add, sub, xor, test with r/m32
    case 0x89: case 0x8B: case 0x8D:  // mov, lea
      return 1 + modrmLength(p + 1);
    case 0x83:  // arithmetic r/m32, imm8
      return 1 + modrmLength(p + 1) + 1;
    case 0x81:  // arithmetic r/m32, imm32
      return 1 + modrmLength(p + 1) + 4;
    case 0xC7:  // mov r/m32, imm32
      return (p[1] >> 3 & 7) == 0 ? 1 + modrmLength(p + 1) + 4 : 0;
    case 0x6A:  // push imm8
      return 2;
    case 0x68:  // push imm32
      return 5;
    default:
      return 0;
  }
}

// Writes into `out` a jump that will sit at `at` and go to `to`.
void writeJump(uint8_t* out, const uint8_t* at, const void* to) {
  out[0] = 0xE9;
  const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - reinterpret_cast<uintptr_t>(at + kJumpSize));
  std::memcpy(out + 1, &rel, sizeof rel);
}

}  // namespace

void* install(void* target, void* hook) {
  auto* code = static_cast<uint8_t*>(target);
  size_t moved = 0;
  while (moved < kJumpSize) {
    const size_t n = movableLength(code + moved);
    if (!n)
      return nullptr;
    moved += n;
  }

  // The trampoline: the moved instructions, then a jump to the rest of the function.
  auto* trampoline =
      static_cast<uint8_t*>(VirtualAlloc(nullptr, moved + kJumpSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!trampoline)
    return nullptr;
  std::memcpy(trampoline, code, moved);
  writeJump(trampoline + moved, trampoline + moved, code + moved);
  DWORD protection;
  VirtualProtect(trampoline, moved + kJumpSize, PAGE_EXECUTE_READ, &protection);
  FlushInstructionCache(GetCurrentProcess(), trampoline, moved + kJumpSize);

  // Over the function's start: the jump to the hook, and int3 for the rest of the moved bytes (never reached).
  if (!VirtualProtect(code, moved, PAGE_EXECUTE_READWRITE, &protection)) {
    VirtualFree(trampoline, 0, MEM_RELEASE);
    return nullptr;
  }
  uint8_t patch[32];
  writeJump(patch, code, hook);
  std::memset(patch + kJumpSize, 0xCC, moved - kJumpSize);
  std::memcpy(code, patch, moved);
  VirtualProtect(code, moved, protection, &protection);
  FlushInstructionCache(GetCurrentProcess(), code, moved);
  return trampoline;
}

}  // namespace detour
