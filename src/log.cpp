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
    // Narrow-mode only. Mixing narrow + wide stdio on the same FILE* is UB
    // under MSVC and reliably crashed earlier versions of this code on first
    // write after _wfopen_s with "ccs=UTF-8".
    _wfopen_s(&g_file, path.c_str(), L"a");
    OutputDebugStringA("tasdll: log_init done\n");
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
        std::fputs(pfx,  g_file);
        std::fputs(body, g_file);
        std::fputs("\n", g_file);
        std::fflush(g_file);
    }
    OutputDebugStringA(pfx);
    OutputDebugStringA(body);
    OutputDebugStringA("\n");
}

void log_wline(const wchar_t* fmt, ...) {
    wchar_t wbody[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(wbody, _countof(wbody), _TRUNCATE, fmt, ap);
    va_end(ap);

    char body[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, wbody, -1, body, (int)sizeof(body), nullptr, nullptr);
    if (n <= 0) {
        body[0] = '?';
        body[1] = 0;
    }
    log_line("%s", body);
}

} // namespace tasdll
