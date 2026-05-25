#include "state.h"

#include <windows.h>
#include <shlobj.h>
#include <string>

namespace tasdll {

State& state() {
    static State s;
    return s;
}

std::wstring default_output_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L"C:\\";
    return base + L"\\Videos\\tasdll";
}

std::wstring foreground_window_title() {
    HWND h = GetForegroundWindow();
    if (!h) return L"";
    wchar_t buf[512];
    int n = GetWindowTextW(h, buf, _countof(buf));
    return (n > 0) ? std::wstring(buf, n) : L"";
}

} // namespace tasdll
