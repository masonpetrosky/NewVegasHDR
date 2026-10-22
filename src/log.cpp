#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace logging {
namespace {

FILE* g_file = nullptr;
std::mutex g_mutex;

}  // namespace

void open(const std::wstring& path) {
  std::lock_guard lock(g_mutex);
  if (!g_file)
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void write(const char* format, ...) {
  std::lock_guard lock(g_mutex);
  if (!g_file)
    return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(g_file, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list args;
  va_start(args, format);
  std::vfprintf(g_file, format, args);
  va_end(args);
  std::fputc('\n', g_file);
  std::fflush(g_file);
}

std::string where(const void* address) {
  char buf[MAX_PATH + 48];
  HMODULE module = nullptr;
  char path[MAX_PATH];
  if (address &&
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(address), &module) &&
      GetModuleFileNameA(module, path, MAX_PATH)) {
    const char* name = std::strrchr(path, '\\');
    std::snprintf(buf, sizeof buf, "%s+0x%x (%p)", name ? name + 1 : path,
                  static_cast<unsigned>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)),
                  address);
  } else {
    std::snprintf(buf, sizeof buf, "%p", address);
  }
  return buf;
}

}  // namespace logging
