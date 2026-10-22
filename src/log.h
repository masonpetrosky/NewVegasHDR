// Plain text log, flushed after every line so that it survives a crash.
#pragma once

#include <string>

namespace logging {

void open(const std::wstring& path);
void write(const char* format, ...);

// A code address as "module+offset (address)", for notes on who calls what.
std::string where(const void* address);

}  // namespace logging

#define LOG(...) logging::write(__VA_ARGS__)
