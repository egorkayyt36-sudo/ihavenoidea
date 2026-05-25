#include "console_cli.h"
#include "gui.h"
#include "log.h"
#include "recorder.h"
#include "speedhack.h"
#include "state.h"

#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

namespace tasdll {

namespace {

std::thread       g_hotkey_thread;
std::atomic<bool> g_hotkey_running{false};
HMODULE           g_self_module = nullptr;

std::wstring this_dll_directory() {
    HMODULE h = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&this_dll_directory), &h);
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(h, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
}

void hotkey_loop() {
    bool prev_f8 = false, prev_f9 = false;
    bool prev_minus = false, prev_plus = false, prev_reset = false;

    while (g_hotkey_running.load() && !state().exit_requested.load()) {
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool f8   = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        bool f9   = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        bool minus = ctrl && (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;     // Ctrl+[
        bool plus  = ctrl && (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;     // Ctrl+]
        bool reset = ctrl && (GetAsyncKeyState(VK_OEM_5) & 0x8000) != 0;     // Ctrl+\

        if (f8 && !prev_f8) {
            bool v = !state().gui_visible.load();
            state().gui_visible.store(v);
            log_line("hotkey: toggle gui -> %d", (int)v);
        }
        if (f9 && !prev_f9) {
            if (recorder_is_active()) recorder_stop();
            else                      recorder_start(speedhack_current_speed());
        }
        if (minus && !prev_minus) {
            double s = speedhack_current_speed();
            speedhack_set_speed(s * 0.5);
        }
        if (plus && !prev_plus) {
            double s = speedhack_current_speed();
            speedhack_set_speed(s * 2.0);
        }
        if (reset && !prev_reset) {
            speedhack_set_speed(1.0);
        }

        prev_f8 = f8; prev_f9 = f9;
        prev_minus = minus; prev_plus = plus; prev_reset = reset;

        Sleep(15);
    }
}

void init_worker() {
    // Give the loader a moment to finish whatever brought us in. This avoids
    // a class of races where an injector resumes the host process right as we
    // try to allocate trampolines / spawn windows.
    Sleep(50);

    std::wstring dir = this_dll_directory();
    log_init(dir + L"\\tasdll.log");
    log_wline(L"tasdll: init in %ls", dir.c_str());

    {
        std::lock_guard<std::mutex> lk(state().cfg_mu);
        state().output_dir = default_output_dir();
    }

    if (!speedhack_install()) {
        log_line("tasdll: speedhack install reported errors; continuing");
    }

    gui_start();
    cli_start();

    g_hotkey_running.store(true);
    g_hotkey_thread = std::thread(hotkey_loop);

    log_line("tasdll: ready");
}

// Detach runs under the loader lock. We must NOT join threads here. Stop the
// child ffmpeg (so the file is finalized), signal worker threads to exit, and
// remove hooks; the process is about to die so we accept that the GUI/CLI
// threads tear themselves down asynchronously.
void shutdown_minimal() {
    state().exit_requested.store(true);
    g_hotkey_running.store(false);
    if (recorder_is_active()) recorder_stop();
    speedhack_uninstall();
    log_line("tasdll: detach");
    log_shutdown();
}

// Plain WinAPI entry — avoids the std::thread/CRT path that can crash if
// invoked from under the loader lock.
DWORD WINAPI init_thread(LPVOID) {
    init_worker();
    return 0;
}

} // anonymous namespace
} // namespace tasdll

BOOL APIENTRY DllMain(HMODULE h_module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(h_module);
        tasdll::g_self_module = h_module;
        // CreateThread + plain function pointer is the safe pattern from DllMain.
        HANDLE t = CreateThread(nullptr, 0, tasdll::init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        break;
    }
    case DLL_PROCESS_DETACH:
        tasdll::shutdown_minimal();
        break;
    }
    return TRUE;
}
