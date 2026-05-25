#include "speedhack.h"

#include "log.h"
#include "state.h"

#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <mutex>

#include <MinHook.h>

namespace tasdll {

namespace {

// Original function pointers.
using PFN_QPC      = BOOL  (WINAPI*)(LARGE_INTEGER*);
using PFN_GTC      = DWORD (WINAPI*)();
using PFN_GTC64    = ULONGLONG (WINAPI*)();
using PFN_TGT      = DWORD (WINAPI*)();
using PFN_GSTAFT   = void  (WINAPI*)(LPFILETIME);
using PFN_GSTPAFT  = void  (WINAPI*)(LPFILETIME);

PFN_QPC      o_QueryPerformanceCounter    = nullptr;
PFN_GTC      o_GetTickCount               = nullptr;
PFN_GTC64    o_GetTickCount64             = nullptr;
PFN_TGT      o_timeGetTime                = nullptr;
PFN_GSTAFT   o_GetSystemTimeAsFileTime    = nullptr;
PFN_GSTPAFT  o_GetSystemTimePreciseAsFileTime = nullptr;

// Anchor structure: each scaled clock holds a (real, fake) pair so we can re-anchor
// when the speed changes without observable jumps.
std::mutex g_anchor_mu;
std::atomic<double> g_speed{1.0};

LONGLONG g_qpc_real_anchor = 0;
LONGLONG g_qpc_fake_anchor = 0;

DWORD    g_gtc_real_anchor   = 0;
DWORD    g_gtc_fake_anchor   = 0;
ULONGLONG g_gtc64_real_anchor = 0;
ULONGLONG g_gtc64_fake_anchor = 0;

DWORD    g_tgt_real_anchor   = 0;
DWORD    g_tgt_fake_anchor   = 0;

ULONGLONG g_ft_real_anchor   = 0; // FILETIME as 100ns ticks
ULONGLONG g_ft_fake_anchor   = 0;
ULONGLONG g_pft_real_anchor  = 0;
ULONGLONG g_pft_fake_anchor  = 0;

inline ULONGLONG filetime_to_uint(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

inline FILETIME uint_to_filetime(ULONGLONG v) {
    FILETIME ft;
    ULARGE_INTEGER u; u.QuadPart = v;
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

// Hook implementations -------------------------------------------------------

BOOL WINAPI hk_QueryPerformanceCounter(LARGE_INTEGER* out) {
    LARGE_INTEGER real;
    if (!o_QueryPerformanceCounter(&real)) return FALSE;
    double speed = g_speed.load(std::memory_order_relaxed);
    LONGLONG elapsed = real.QuadPart - g_qpc_real_anchor;
    LONGLONG fake = g_qpc_fake_anchor + (LONGLONG)((double)elapsed * speed);
    if (out) out->QuadPart = fake;
    return TRUE;
}

DWORD WINAPI hk_GetTickCount() {
    DWORD real = o_GetTickCount();
    double speed = g_speed.load(std::memory_order_relaxed);
    DWORD elapsed = real - g_gtc_real_anchor;
    return g_gtc_fake_anchor + (DWORD)((double)elapsed * speed);
}

ULONGLONG WINAPI hk_GetTickCount64() {
    ULONGLONG real = o_GetTickCount64();
    double speed = g_speed.load(std::memory_order_relaxed);
    ULONGLONG elapsed = real - g_gtc64_real_anchor;
    return g_gtc64_fake_anchor + (ULONGLONG)((double)elapsed * speed);
}

DWORD WINAPI hk_timeGetTime() {
    DWORD real = o_timeGetTime();
    double speed = g_speed.load(std::memory_order_relaxed);
    DWORD elapsed = real - g_tgt_real_anchor;
    return g_tgt_fake_anchor + (DWORD)((double)elapsed * speed);
}

void WINAPI hk_GetSystemTimeAsFileTime(LPFILETIME out) {
    FILETIME ft;
    o_GetSystemTimeAsFileTime(&ft);
    if (!out) return;
    double speed = g_speed.load(std::memory_order_relaxed);
    ULONGLONG real = filetime_to_uint(ft);
    ULONGLONG elapsed = real - g_ft_real_anchor;
    ULONGLONG fake = g_ft_fake_anchor + (ULONGLONG)((double)elapsed * speed);
    *out = uint_to_filetime(fake);
}

void WINAPI hk_GetSystemTimePreciseAsFileTime(LPFILETIME out) {
    FILETIME ft;
    o_GetSystemTimePreciseAsFileTime(&ft);
    if (!out) return;
    double speed = g_speed.load(std::memory_order_relaxed);
    ULONGLONG real = filetime_to_uint(ft);
    ULONGLONG elapsed = real - g_pft_real_anchor;
    ULONGLONG fake = g_pft_fake_anchor + (ULONGLONG)((double)elapsed * speed);
    *out = uint_to_filetime(fake);
}

// Initial anchor setup ------------------------------------------------------

void seed_anchors_locked() {
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    g_qpc_real_anchor = qpc.QuadPart;
    g_qpc_fake_anchor = qpc.QuadPart;

    DWORD gtc = GetTickCount();
    g_gtc_real_anchor = gtc;
    g_gtc_fake_anchor = gtc;

    ULONGLONG gtc64 = GetTickCount64();
    g_gtc64_real_anchor = gtc64;
    g_gtc64_fake_anchor = gtc64;

    DWORD tgt = timeGetTime();
    g_tgt_real_anchor = tgt;
    g_tgt_fake_anchor = tgt;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    g_ft_real_anchor = filetime_to_uint(ft);
    g_ft_fake_anchor = g_ft_real_anchor;

    GetSystemTimePreciseAsFileTime(&ft);
    g_pft_real_anchor = filetime_to_uint(ft);
    g_pft_fake_anchor = g_pft_real_anchor;
}

bool g_installed = false;

template <typename T>
bool hook_one(const wchar_t* mod, const char* func, T detour, T& out_orig) {
    HMODULE h = GetModuleHandleW(mod);
    if (!h) {
        log_line("speedhack: module %ls not loaded", mod);
        return false;
    }
    void* target = (void*)GetProcAddress(h, func);
    if (!target) {
        log_line("speedhack: %s not found in %ls", func, mod);
        return false;
    }
    LPVOID orig = nullptr;
    MH_STATUS s = MH_CreateHook(target, (LPVOID)detour, &orig);
    if (s != MH_OK) {
        log_line("speedhack: MH_CreateHook(%s) failed: %d", func, (int)s);
        return false;
    }
    out_orig = reinterpret_cast<T>(orig);
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        log_line("speedhack: MH_EnableHook(%s) failed: %d", func, (int)s);
        return false;
    }
    log_line("speedhack: hooked %s", func);
    return true;
}

} // anonymous namespace

bool speedhack_install() {
    if (g_installed) return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("speedhack: MH_Initialize failed: %d", (int)s);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_anchor_mu);
        seed_anchors_locked();
    }

    // Resolve once per name. On Windows 7+ many of these are implemented in
    // kernelbase.dll and re-exported by kernel32.dll. Hooking kernelbase
    // catches callers that came via api-ms-win-core-* forwarders (Unity / .NET
    // commonly take that path). Hooking kernel32 catches direct importers.
    // If kernel32 and kernelbase point at the same address (forwarder), the
    // second hook attempt is a no-op handled by MinHook.
    bool any = false;
    auto try_pair = [&](const char* func, auto detour, auto& out_orig) {
        if (!out_orig) {
            if (hook_one(L"kernelbase.dll", func, detour, out_orig)) { any = true; return; }
        }
        if (!out_orig) {
            if (hook_one(L"kernel32.dll", func, detour, out_orig)) any = true;
        }
    };

    try_pair("QueryPerformanceCounter",        &hk_QueryPerformanceCounter,        o_QueryPerformanceCounter);
    try_pair("GetTickCount",                   &hk_GetTickCount,                   o_GetTickCount);
    try_pair("GetTickCount64",                 &hk_GetTickCount64,                 o_GetTickCount64);
    try_pair("GetSystemTimeAsFileTime",        &hk_GetSystemTimeAsFileTime,        o_GetSystemTimeAsFileTime);
    try_pair("GetSystemTimePreciseAsFileTime", &hk_GetSystemTimePreciseAsFileTime, o_GetSystemTimePreciseAsFileTime);

    if (!GetModuleHandleW(L"winmm.dll")) LoadLibraryW(L"winmm.dll");
    if (hook_one(L"winmm.dll", "timeGetTime", &hk_timeGetTime, o_timeGetTime)) any = true;

    g_installed = true;
    log_line("speedhack: install %s", any ? "ok" : "no hooks installed");
    return any;
}

void speedhack_uninstall() {
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed = false;
    log_line("speedhack: uninstalled");
}

void speedhack_set_speed(double new_speed) {
    if (new_speed <= 0.0) new_speed = 0.0001;
    if (new_speed > 100.0) new_speed = 100.0;

    std::lock_guard<std::mutex> lk(g_anchor_mu);
    double old_speed = g_speed.load();

    if (o_QueryPerformanceCounter) {
        LARGE_INTEGER real; o_QueryPerformanceCounter(&real);
        LONGLONG elapsed = real.QuadPart - g_qpc_real_anchor;
        g_qpc_fake_anchor += (LONGLONG)((double)elapsed * old_speed);
        g_qpc_real_anchor  = real.QuadPart;
    }
    if (o_GetTickCount) {
        DWORD real = o_GetTickCount();
        DWORD elapsed = real - g_gtc_real_anchor;
        g_gtc_fake_anchor += (DWORD)((double)elapsed * old_speed);
        g_gtc_real_anchor  = real;
    }
    if (o_GetTickCount64) {
        ULONGLONG real = o_GetTickCount64();
        ULONGLONG elapsed = real - g_gtc64_real_anchor;
        g_gtc64_fake_anchor += (ULONGLONG)((double)elapsed * old_speed);
        g_gtc64_real_anchor  = real;
    }
    if (o_timeGetTime) {
        DWORD real = o_timeGetTime();
        DWORD elapsed = real - g_tgt_real_anchor;
        g_tgt_fake_anchor += (DWORD)((double)elapsed * old_speed);
        g_tgt_real_anchor  = real;
    }
    if (o_GetSystemTimeAsFileTime) {
        FILETIME ft; o_GetSystemTimeAsFileTime(&ft);
        ULONGLONG real = filetime_to_uint(ft);
        ULONGLONG elapsed = real - g_ft_real_anchor;
        g_ft_fake_anchor += (ULONGLONG)((double)elapsed * old_speed);
        g_ft_real_anchor  = real;
    }
    if (o_GetSystemTimePreciseAsFileTime) {
        FILETIME ft; o_GetSystemTimePreciseAsFileTime(&ft);
        ULONGLONG real = filetime_to_uint(ft);
        ULONGLONG elapsed = real - g_pft_real_anchor;
        g_pft_fake_anchor += (ULONGLONG)((double)elapsed * old_speed);
        g_pft_real_anchor  = real;
    }

    g_speed.store(new_speed, std::memory_order_release);
    state().speed.store(new_speed);
    log_line("speedhack: speed %.4f -> %.4f", old_speed, new_speed);
}

double speedhack_current_speed() {
    return g_speed.load(std::memory_order_relaxed);
}

} // namespace tasdll
