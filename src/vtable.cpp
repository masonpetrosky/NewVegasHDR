#include "vtable.h"

#include <windows.h>

namespace vt {

void* patch(void* object, int index, void* hook) {
  void** slot = *reinterpret_cast<void***>(object) + index;
  if (*slot == hook)
    return nullptr;
  DWORD protect;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protect))
    return nullptr;
  void* previous = *slot;
  *slot = hook;
  VirtualProtect(slot, sizeof(void*), protect, &protect);
  return previous;
}

}  // namespace vt
