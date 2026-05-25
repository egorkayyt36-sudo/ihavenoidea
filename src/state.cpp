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

static bool window_is_ours(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return false;
    wchar_t cls[128] = {0};
    GetClassNameW(h, cls, _countof(cls));
    if (wcscmp(cls, L"tasdll_imgui_wnd") == 0) return true;
    if (wcscmp(cls, L"ConsoleWindowClass") == 0) return true;
    if (wcsstr(cls, L"PseudoConsoleWindow")) return true;
    return false;
}

struct EnumCtx { HWND best = nullptr; };

static BOOL CALLBACK pick_target_proc(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE; // skip child/owned
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (window_is_ours(h)) return TRUE;
    wchar_t t[8] = {0};
    if (GetWindowTextW(h, t, 1) <= 0) return TRUE; // skip empty titles
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    ctx->best = h;
    return FALSE;
}

std::wstring foreground_window_title() {
    HWND h = GetForegroundWindow();
    if (h && !window_is_ours(h)) {
        wchar_t buf[512];
        int n = GetWindowTextW(h, buf, _countof(buf));
        if (n > 0) return std::wstring(buf, n);
    }
    // Foreground was ours (or empty) — pick the topmost non-tasdll, non-tool
    // window from the Z-order.
    EnumCtx ctx;
    EnumWindows(pick_target_proc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.best) {
        wchar_t buf[512];
        int n = GetWindowTextW(ctx.best, buf, _countof(buf));
        if (n > 0) return std::wstring(buf, n);
    }
    return L"";
}

} // namespace tasdll
