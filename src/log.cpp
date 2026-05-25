#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace tasdll {

namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
}

void log_init(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) return;
    _wfopen_s(&g_file, path.c_str(), L"a+, ccs=UTF-8");
}

void log_shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

static void prefix(char* buf, size_t n) {
    SYSTEMTIME st; GetLocalTime(&st);
    _snprintf_s(buf, n, _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void log_line(const char* fmt, ...) {
    char pfx[64]; prefix(pfx, sizeof(pfx));
    char body[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) {
        std::fprintf(g_file, "%s%s\n", pfx, body);
        std::fflush(g_file);
    }
    OutputDebugStringA(pfx);
    OutputDebugStringA(body);
    OutputDebugStringA("\n");
}

void log_wline(const wchar_t* fmt, ...) {
    char pfx[64]; prefix(pfx, sizeof(pfx));
    wchar_t body[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) {
        std::fprintf(g_file, "%s", pfx);
        std::fputws(body, g_file);
        std::fputws(L"\n", g_file);
        std::fflush(g_file);
    }
    OutputDebugStringA(pfx);
    OutputDebugStringW(body);
    OutputDebugStringW(L"\n");
}

} // namespace tasdll
