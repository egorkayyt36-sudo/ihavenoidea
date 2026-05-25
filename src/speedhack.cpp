#include "speedhack.h"

#include "log.h"
#include "state.h"

#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>

#include <MinHook.h>

namespace tasdll {

namespace {

using PFN_QPC     = BOOL  (WINAPI*)(LARGE_INTEGER*);
using PFN_GTC     = DWORD (WINAPI*)();
using PFN_GTC64   = ULONGLONG (WINAPI*)();
using PFN_TGT     = DWORD (WINAPI*)();
using PFN_GSTAFT  = void  (WINAPI*)(LPFILETIME);

// Per-hook trampoline slot. Each installed hook gets its own trampoline; the
// detour for that slot calls only its own trampoline. This lets us install
// hooks on multiple modules (kernel32 + kernelbase) safely even if they're
// distinct implementations.
struct Slot { void* trampoline = nullptr; std::atomic<uint64_t> hits{0}; };

// QPC has multiple potential hook sites.
Slot s_QPC_kb, s_QPC_k32;
// GetTickCount
Slot s_GTC_kb, s_GTC_k32;
// GetTickCount64
Slot s_GTC64_kb, s_GTC64_k32;
// timeGetTime (winmm only)
Slot s_TGT;
// GetSystemTimeAsFileTime
Slot s_GSTAFT_kb, s_GSTAFT_k32;
// GetSystemTimePreciseAsFileTime
Slot s_GSTPAFT_kb, s_GSTPAFT_k32;

// Canonical "original" pointers used by speedhack_set_speed (read the real
// current time without scaling). First successful hook of each function
// captures its trampoline here.
PFN_QPC     o_QPC_canonical    = nullptr;
PFN_GTC     o_GTC_canonical    = nullptr;
PFN_GTC64   o_GTC64_canonical  = nullptr;
PFN_TGT     o_TGT_canonical    = nullptr;
PFN_GSTAFT  o_GSTAFT_canonical = nullptr;
PFN_GSTAFT  o_GSTPAFT_canonical = nullptr;

// Fake-clock state ---------------------------------------------------------

std::mutex          g_anchor_mu;
std::atomic<double> g_speed{1.0};

LONGLONG   g_qpc_real_anchor = 0;
LONGLONG   g_qpc_fake_anchor = 0;

DWORD      g_gtc_real_anchor = 0;
DWORD      g_gtc_fake_anchor = 0;

ULONGLONG  g_gtc64_real_anchor = 0;
ULONGLONG  g_gtc64_fake_anchor = 0;

DWORD      g_tgt_real_anchor = 0;
DWORD      g_tgt_fake_anchor = 0;

ULONGLONG  g_ft_real_anchor  = 0;
ULONGLONG  g_ft_fake_anchor  = 0;
ULONGLONG  g_pft_real_anchor = 0;
ULONGLONG  g_pft_fake_anchor = 0;

inline ULONGLONG ft_to_u(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}
inline FILETIME u_to_ft(ULONGLONG v) {
    FILETIME ft; ULARGE_INTEGER u; u.QuadPart = v;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    return ft;
}

// Re-entry guard. If kernel32's QPC forwards to kernelbase's QPC and both are
// hooked, the outer detour would otherwise scale the already-scaled value
// returned by the inner detour. We let the OUTER (first-entered) detour scale
// and inner detours pass through to their raw trampoline.
thread_local int t_in_hook = 0;

struct Reentry {
    bool outer;
    Reentry() : outer(t_in_hook == 0) { ++t_in_hook; }
    ~Reentry() { --t_in_hook; }
};

// Shared bodies -----------------------------------------------------------

BOOL qpc_body(Slot& slot, LARGE_INTEGER* out) {
    auto orig = (PFN_QPC)slot.trampoline;
    Reentry r;
    LARGE_INTEGER real;
    if (!orig(&real)) return FALSE;
    if (!r.outer) { if (out) *out = real; return TRUE; }
    slot.hits.fetch_add(1, std::memory_order_relaxed);
    double speed = g_speed.load(std::memory_order_relaxed);
    LONGLONG elapsed = real.QuadPart - g_qpc_real_anchor;
    LONGLONG fake = g_qpc_fake_anchor + (LONGLONG)((double)elapsed * speed);
    if (out) out->QuadPart = fake;
    return TRUE;
}
DWORD gtc_body(Slot& slot) {
    auto orig = (PFN_GTC)slot.trampoline;
    Reentry r;
    DWORD real = orig();
    if (!r.outer) return real;
    slot.hits.fetch_add(1, std::memory_order_relaxed);
    double speed = g_speed.load(std::memory_order_relaxed);
    DWORD elapsed = real - g_gtc_real_anchor;
    return g_gtc_fake_anchor + (DWORD)((double)elapsed * speed);
}
ULONGLONG gtc64_body(Slot& slot) {
    auto orig = (PFN_GTC64)slot.trampoline;
    Reentry r;
    ULONGLONG real = orig();
    if (!r.outer) return real;
    slot.hits.fetch_add(1, std::memory_order_relaxed);
    double speed = g_speed.load(std::memory_order_relaxed);
    ULONGLONG elapsed = real - g_gtc64_real_anchor;
    return g_gtc64_fake_anchor + (ULONGLONG)((double)elapsed * speed);
}
DWORD tgt_body(Slot& slot) {
    auto orig = (PFN_TGT)slot.trampoline;
    Reentry r;
    DWORD real = orig();
    if (!r.outer) return real;
    slot.hits.fetch_add(1, std::memory_order_relaxed);
    double speed = g_speed.load(std::memory_order_relaxed);
    DWORD elapsed = real - g_tgt_real_anchor;
    return g_tgt_fake_anchor + (DWORD)((double)elapsed * speed);
}
void gstaft_body(Slot& slot, LPFILETIME out, ULONGLONG& real_anchor, ULONGLONG& fake_anchor) {
    auto orig = (PFN_GSTAFT)slot.trampoline;
    Reentry r;
    FILETIME ft; orig(&ft);
    if (!out) return;
    if (!r.outer) { *out = ft; return; }
    slot.hits.fetch_add(1, std::memory_order_relaxed);
    double speed = g_speed.load(std::memory_order_relaxed);
    ULONGLONG real = ft_to_u(ft);
    ULONGLONG elapsed = real - real_anchor;
    ULONGLONG fake = fake_anchor + (ULONGLONG)((double)elapsed * speed);
    *out = u_to_ft(fake);
}

// Per-module detours -------------------------------------------------------

BOOL WINAPI hk_QPC_kb (LARGE_INTEGER* o) { return qpc_body(s_QPC_kb,  o); }
BOOL WINAPI hk_QPC_k32(LARGE_INTEGER* o) { return qpc_body(s_QPC_k32, o); }

DWORD WINAPI hk_GTC_kb () { return gtc_body(s_GTC_kb);  }
DWORD WINAPI hk_GTC_k32() { return gtc_body(s_GTC_k32); }

ULONGLONG WINAPI hk_GTC64_kb () { return gtc64_body(s_GTC64_kb);  }
ULONGLONG WINAPI hk_GTC64_k32() { return gtc64_body(s_GTC64_k32); }

DWORD WINAPI hk_TGT() { return tgt_body(s_TGT); }

void WINAPI hk_GSTAFT_kb (LPFILETIME o) { gstaft_body(s_GSTAFT_kb,  o, g_ft_real_anchor, g_ft_fake_anchor);  }
void WINAPI hk_GSTAFT_k32(LPFILETIME o) { gstaft_body(s_GSTAFT_k32, o, g_ft_real_anchor, g_ft_fake_anchor); }

void WINAPI hk_GSTPAFT_kb (LPFILETIME o) { gstaft_body(s_GSTPAFT_kb,  o, g_pft_real_anchor, g_pft_fake_anchor); }
void WINAPI hk_GSTPAFT_k32(LPFILETIME o) { gstaft_body(s_GSTPAFT_k32, o, g_pft_real_anchor, g_pft_fake_anchor); }

// Anchor seed --------------------------------------------------------------

void seed_anchors_locked() {
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    g_qpc_real_anchor = qpc.QuadPart;
    g_qpc_fake_anchor = qpc.QuadPart;

    DWORD gtc = GetTickCount();
    g_gtc_real_anchor = gtc; g_gtc_fake_anchor = gtc;

    ULONGLONG gtc64 = GetTickCount64();
    g_gtc64_real_anchor = gtc64; g_gtc64_fake_anchor = gtc64;

    DWORD tgt = timeGetTime();
    g_tgt_real_anchor = tgt; g_tgt_fake_anchor = tgt;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    g_ft_real_anchor = ft_to_u(ft); g_ft_fake_anchor = g_ft_real_anchor;

    GetSystemTimePreciseAsFileTime(&ft);
    g_pft_real_anchor = ft_to_u(ft); g_pft_fake_anchor = g_pft_real_anchor;
}

bool g_installed = false;

bool install_one(const wchar_t* mod, const char* func, void* detour,
                 Slot& slot, void** canonical_slot = nullptr) {
    HMODULE h = GetModuleHandleW(mod);
    if (!h) {
        log_line("speedhack: module %ls not loaded, skipping %s", mod, func);
        return false;
    }
    void* target = (void*)GetProcAddress(h, func);
    if (!target) {
        log_line("speedhack: %ls!%s not exported", mod, func);
        return false;
    }
    LPVOID orig = nullptr;
    MH_STATUS s = MH_CreateHook(target, detour, &orig);
    if (s == MH_ERROR_ALREADY_CREATED) {
        // Same target address as a previously-installed hook (kernel32 export
        // is a JMP into kernelbase pointing at the same location). That's
        // fine; the existing hook will catch this caller too.
        log_line("speedhack: %ls!%s shares target with an existing hook, ok", mod, func);
        return false; // we didn't install a new one, but it's covered
    }
    if (s != MH_OK) {
        log_line("speedhack: MH_CreateHook(%ls!%s) failed: %d", mod, func, (int)s);
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        log_line("speedhack: MH_EnableHook(%ls!%s) failed: %d", mod, func, (int)s);
        return false;
    }
    slot.trampoline = orig;
    if (canonical_slot && !*canonical_slot) *canonical_slot = orig;
    log_line("speedhack: hooked %ls!%s @ %p", mod, func, target);
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

    // Make sure winmm is loaded so timeGetTime is hookable.
    if (!GetModuleHandleW(L"winmm.dll")) LoadLibraryW(L"winmm.dll");

    // Hook both kernelbase and kernel32 for each function. MinHook will fail
    // with MH_ERROR_ALREADY_CREATED if the second target shares its address
    // with the first (typical when kernel32 is a forwarder), which we treat
    // as success-equivalent.
    int installed = 0;

    if (install_one(L"kernelbase.dll", "QueryPerformanceCounter",
                    (void*)&hk_QPC_kb, s_QPC_kb,
                    (void**)&o_QPC_canonical)) installed++;
    if (install_one(L"kernel32.dll",   "QueryPerformanceCounter",
                    (void*)&hk_QPC_k32, s_QPC_k32,
                    (void**)&o_QPC_canonical)) installed++;

    if (install_one(L"kernelbase.dll", "GetTickCount",
                    (void*)&hk_GTC_kb, s_GTC_kb,
                    (void**)&o_GTC_canonical)) installed++;
    if (install_one(L"kernel32.dll",   "GetTickCount",
                    (void*)&hk_GTC_k32, s_GTC_k32,
                    (void**)&o_GTC_canonical)) installed++;

    if (install_one(L"kernelbase.dll", "GetTickCount64",
                    (void*)&hk_GTC64_kb, s_GTC64_kb,
                    (void**)&o_GTC64_canonical)) installed++;
    if (install_one(L"kernel32.dll",   "GetTickCount64",
                    (void*)&hk_GTC64_k32, s_GTC64_k32,
                    (void**)&o_GTC64_canonical)) installed++;

    if (install_one(L"kernelbase.dll", "GetSystemTimeAsFileTime",
                    (void*)&hk_GSTAFT_kb, s_GSTAFT_kb,
                    (void**)&o_GSTAFT_canonical)) installed++;
    if (install_one(L"kernel32.dll",   "GetSystemTimeAsFileTime",
                    (void*)&hk_GSTAFT_k32, s_GSTAFT_k32,
                    (void**)&o_GSTAFT_canonical)) installed++;

    if (install_one(L"kernelbase.dll", "GetSystemTimePreciseAsFileTime",
                    (void*)&hk_GSTPAFT_kb, s_GSTPAFT_kb,
                    (void**)&o_GSTPAFT_canonical)) installed++;
    if (install_one(L"kernel32.dll",   "GetSystemTimePreciseAsFileTime",
                    (void*)&hk_GSTPAFT_k32, s_GSTPAFT_k32,
                    (void**)&o_GSTPAFT_canonical)) installed++;

    if (install_one(L"winmm.dll", "timeGetTime",
                    (void*)&hk_TGT, s_TGT,
                    (void**)&o_TGT_canonical)) installed++;

    g_installed = true;
    log_line("speedhack: install complete, %d hook(s) active", installed);
    return installed > 0;
}

void speedhack_uninstall() {
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed = false;
    log_line("speedhack: uninstalled");
}

void speedhack_set_speed(double new_speed) {
    if (new_speed <= 0.0)  new_speed = 0.0001;
    if (new_speed > 100.0) new_speed = 100.0;

    std::lock_guard<std::mutex> lk(g_anchor_mu);
    double old_speed = g_speed.load();

    if (o_QPC_canonical) {
        LARGE_INTEGER real; o_QPC_canonical(&real);
        LONGLONG elapsed = real.QuadPart - g_qpc_real_anchor;
        g_qpc_fake_anchor += (LONGLONG)((double)elapsed * old_speed);
        g_qpc_real_anchor  = real.QuadPart;
    }
    if (o_GTC_canonical) {
        DWORD real = o_GTC_canonical();
        DWORD elapsed = real - g_gtc_real_anchor;
        g_gtc_fake_anchor += (DWORD)((double)elapsed * old_speed);
        g_gtc_real_anchor  = real;
    }
    if (o_GTC64_canonical) {
        ULONGLONG real = o_GTC64_canonical();
        ULONGLONG elapsed = real - g_gtc64_real_anchor;
        g_gtc64_fake_anchor += (ULONGLONG)((double)elapsed * old_speed);
        g_gtc64_real_anchor  = real;
    }
    if (o_TGT_canonical) {
        DWORD real = o_TGT_canonical();
        DWORD elapsed = real - g_tgt_real_anchor;
        g_tgt_fake_anchor += (DWORD)((double)elapsed * old_speed);
        g_tgt_real_anchor  = real;
    }
    if (o_GSTAFT_canonical) {
        FILETIME ft; o_GSTAFT_canonical(&ft);
        ULONGLONG real = ft_to_u(ft);
        ULONGLONG elapsed = real - g_ft_real_anchor;
        g_ft_fake_anchor += (ULONGLONG)((double)elapsed * old_speed);
        g_ft_real_anchor  = real;
    }
    if (o_GSTPAFT_canonical) {
        FILETIME ft; o_GSTPAFT_canonical(&ft);
        ULONGLONG real = ft_to_u(ft);
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

std::string speedhack_stats() {
    std::ostringstream os;
    os << "speedhack stats (current speed " << g_speed.load() << "x):\n";
    auto row = [&](const char* name, const Slot& s) {
        os << "  " << name << ": "
           << (s.trampoline ? "INSTALLED" : "off")
           << "  hits=" << s.hits.load() << "\n";
    };
    row("kernelbase!QueryPerformanceCounter",          s_QPC_kb);
    row("kernel32!QueryPerformanceCounter",            s_QPC_k32);
    row("kernelbase!GetTickCount",                     s_GTC_kb);
    row("kernel32!GetTickCount",                       s_GTC_k32);
    row("kernelbase!GetTickCount64",                   s_GTC64_kb);
    row("kernel32!GetTickCount64",                     s_GTC64_k32);
    row("kernelbase!GetSystemTimeAsFileTime",          s_GSTAFT_kb);
    row("kernel32!GetSystemTimeAsFileTime",            s_GSTAFT_k32);
    row("kernelbase!GetSystemTimePreciseAsFileTime",   s_GSTPAFT_kb);
    row("kernel32!GetSystemTimePreciseAsFileTime",     s_GSTPAFT_k32);
    row("winmm!timeGetTime",                           s_TGT);
    return os.str();
}

} // namespace tasdll
