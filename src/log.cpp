#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
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

}  // namespace logging
