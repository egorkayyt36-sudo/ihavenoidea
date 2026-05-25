#pragma once

#include <string>

namespace tasdll {

void log_init(const std::wstring& log_path);
void log_shutdown();

void log_line(const char* fmt, ...);
void log_wline(const wchar_t* fmt, ...);

} // namespace tasdll
