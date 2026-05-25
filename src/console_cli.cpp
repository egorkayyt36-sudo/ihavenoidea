#include "console_cli.h"

#include "log.h"
#include "recorder.h"
#include "speedhack.h"
#include "state.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace tasdll {

namespace {

std::thread       g_thread;
std::atomic<bool> g_running{false};
bool              g_console_owned = false;

void print_help() {
    std::puts(
        "tasdll commands:\n"
        "  speed <x>        set speed multiplier (e.g. 0.5, 2.0)\n"
        "  reset            speed back to 1.0\n"
        "  rec start        start recording (snapshots current speed)\n"
        "  rec stop         stop recording\n"
        "  title <name>     set capture window title\n"
        "  title fg         use current foreground window title\n"
        "  desktop on|off   capture whole desktop instead of a window\n"
        "  audio on|off     toggle dshow audio capture\n"
        "  fps <n>          set capture framerate\n"
        "  out <dir>        set output directory\n"
        "  show|hide        toggle GUI window\n"
        "  status           show current settings\n"
        "  stats            speedhack hook hit-counts (verify hooks fire)\n"
        "  help             print this\n"
        "  quit             exit CLI loop\n");
}

void print_status() {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.cfg_mu);
    char out_dir[600] = {0};
    char title[300]   = {0};
    char ffmpeg[600]  = {0};
    WideCharToMultiByte(CP_UTF8, 0, s.output_dir.c_str(),  -1, out_dir, sizeof(out_dir), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, s.window_title.c_str(),-1, title,   sizeof(title),   nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, s.ffmpeg_path.c_str(), -1, ffmpeg,  sizeof(ffmpeg),  nullptr, nullptr);
    std::printf("speed     : %.4fx\n", speedhack_current_speed());
    std::printf("recording : %s\n", recorder_is_active() ? "yes" : "no");
    std::printf("output    : %s\n", out_dir);
    std::printf("title     : %s\n", title);
    std::printf("desktop   : %s\n", s.use_desktop ? "yes" : "no");
    std::printf("audio     : %s\n", s.capture_audio ? "yes" : "no");
    std::printf("fps       : %d\n", s.capture_fps);
    std::printf("ffmpeg    : %s\n", ffmpeg[0] ? ffmpeg : "ffmpeg (from PATH)");
}

void process_line(const char* line) {
    while (*line == ' ' || *line == '\t') ++line;
    if (!*line) return;

    char cmd[64] = {0};
    int  n = 0;
    if (sscanf_s(line, "%63s%n", cmd, (unsigned)sizeof(cmd), &n) != 1) return;
    const char* rest = line + n;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (_stricmp(cmd, "help") == 0) { print_help(); return; }
    if (_stricmp(cmd, "status") == 0) { print_status(); return; }
    if (_stricmp(cmd, "stats") == 0) { std::fputs(speedhack_stats().c_str(), stdout); return; }
    if (_stricmp(cmd, "speed") == 0) {
        double v = atof(rest);
        if (v <= 0.0) { std::puts("speed must be > 0"); return; }
        speedhack_set_speed(v);
        std::printf("speed = %.4fx\n", speedhack_current_speed());
        return;
    }
    if (_stricmp(cmd, "reset") == 0) {
        speedhack_set_speed(1.0);
        std::puts("speed = 1.0x");
        return;
    }
    if (_stricmp(cmd, "rec") == 0) {
        if (_strnicmp(rest, "start", 5) == 0) {
            if (recorder_start(speedhack_current_speed())) std::puts("recording started");
            else {
                char buf[300];
                WideCharToMultiByte(CP_UTF8, 0, recorder_last_error().c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
                std::printf("recording failed: %s\n", buf);
            }
        } else if (_strnicmp(rest, "stop", 4) == 0) {
            recorder_stop();
            std::puts("recording stopped");
        } else {
            std::puts("usage: rec start|stop");
        }
        return;
    }
    if (_stricmp(cmd, "title") == 0) {
        if (_stricmp(rest, "fg") == 0) {
            std::lock_guard<std::mutex> lk(state().cfg_mu);
            state().window_title = foreground_window_title();
            char buf[300];
            WideCharToMultiByte(CP_UTF8, 0, state().window_title.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
            std::printf("title = %s\n", buf);
        } else {
            wchar_t wbuf[300];
            MultiByteToWideChar(CP_UTF8, 0, rest, -1, wbuf, _countof(wbuf));
            std::lock_guard<std::mutex> lk(state().cfg_mu);
            state().window_title = wbuf;
        }
        return;
    }
    if (_stricmp(cmd, "desktop") == 0) {
        std::lock_guard<std::mutex> lk(state().cfg_mu);
        state().use_desktop = (_stricmp(rest, "on") == 0 || _stricmp(rest, "true") == 0 || _stricmp(rest, "1") == 0);
        std::printf("desktop = %s\n", state().use_desktop ? "on" : "off");
        return;
    }
    if (_stricmp(cmd, "audio") == 0) {
        std::lock_guard<std::mutex> lk(state().cfg_mu);
        state().capture_audio = (_stricmp(rest, "on") == 0 || _stricmp(rest, "true") == 0 || _stricmp(rest, "1") == 0);
        std::printf("audio = %s\n", state().capture_audio ? "on" : "off");
        return;
    }
    if (_stricmp(cmd, "fps") == 0) {
        int v = atoi(rest);
        if (v < 1 || v > 240) { std::puts("fps must be in [1,240]"); return; }
        std::lock_guard<std::mutex> lk(state().cfg_mu);
        state().capture_fps = v;
        std::printf("fps = %d\n", v);
        return;
    }
    if (_stricmp(cmd, "out") == 0) {
        wchar_t wbuf[600];
        MultiByteToWideChar(CP_UTF8, 0, rest, -1, wbuf, _countof(wbuf));
        std::lock_guard<std::mutex> lk(state().cfg_mu);
        state().output_dir = wbuf;
        std::printf("out = %s\n", rest);
        return;
    }
    if (_stricmp(cmd, "show") == 0) { state().gui_visible.store(true);  std::puts("gui shown"); return; }
    if (_stricmp(cmd, "hide") == 0) { state().gui_visible.store(false); std::puts("gui hidden"); return; }
    if (_stricmp(cmd, "quit") == 0 || _stricmp(cmd, "exit") == 0) {
        g_running.store(false);
        return;
    }
    std::puts("unknown command (try 'help')");
}

void thread_main() {
    if (!GetConsoleWindow()) {
        if (AllocConsole()) {
            g_console_owned = true;
            FILE* f;
            freopen_s(&f, "CONIN$",  "r", stdin);
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            SetConsoleTitleW(L"tasdll");
        }
    }

    std::puts("tasdll CLI ready. Type 'help'.");
    print_status();

    char line[1024];
    while (g_running.load() && !state().exit_requested.load()) {
        std::fputs("tasdll> ", stdout);
        std::fflush(stdout);
        if (!std::fgets(line, sizeof(line), stdin)) {
            Sleep(50);
            continue;
        }
        size_t n = std::strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        process_line(line);
    }

    if (g_console_owned) {
        FreeConsole();
        g_console_owned = false;
    }
}

} // anonymous namespace

bool cli_start() {
    if (g_running.exchange(true)) return true;
    g_thread = std::thread(thread_main);
    return true;
}

void cli_stop() {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
}

} // namespace tasdll
