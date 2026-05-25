#include "recorder.h"

#include "log.h"
#include "state.h"

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace tasdll {

namespace {

std::mutex          g_mu;
PROCESS_INFORMATION g_pi{};
HANDLE              g_stdin_w = nullptr; // we write 'q' to ask ffmpeg to finalize cleanly
std::atomic<bool>   g_active{false};
std::wstring        g_last_output;
std::wstring        g_last_error;

bool ensure_directory(const std::wstring& dir) {
    int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

std::wstring timestamped_filename(const std::wstring& dir) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"tasdll_%04u%02u%02u_%02u%02u%02u.mp4",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return dir + L"\\" + name;
}

// Build a chained atempo filter so audio is also corrected. atempo accepts
// values in [0.5, 100.0]; chain multiple stages for extreme values.
std::wstring build_atempo_chain(double speed) {
    if (speed <= 0.0) return L"atempo=1.0";
    double target = speed;
    if (target >= 0.5 && target <= 100.0) {
        wchar_t buf[64];
        swprintf_s(buf, L"atempo=%.6f", target);
        return buf;
    }
    std::wstring chain;
    while (target < 0.5) {
        chain += (chain.empty() ? L"atempo=0.5" : L",atempo=0.5");
        target /= 0.5;
    }
    while (target > 100.0) {
        chain += (chain.empty() ? L"atempo=100.0" : L",atempo=100.0");
        target /= 100.0;
    }
    wchar_t buf[64];
    swprintf_s(buf, L",atempo=%.6f", target);
    chain += buf;
    return chain;
}

std::wstring escape_for_cmdline(const std::wstring& s) {
    // Minimal escaping for double quotes inside an argument we'll wrap in quotes.
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    return out;
}

std::wstring build_command(double speed, const std::wstring& output_path) {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.cfg_mu);

    std::wstring ffmpeg = s.ffmpeg_path.empty() ? L"ffmpeg" : s.ffmpeg_path;
    int fps = s.capture_fps > 0 ? s.capture_fps : 60;
    bool audio = s.capture_audio;
    bool desktop = s.use_desktop;
    std::wstring title = s.window_title;

    if (title.empty() && !desktop) {
        title = foreground_window_title();
    }

    std::wstring setpts;
    {
        wchar_t buf[64];
        swprintf_s(buf, L"setpts=PTS*%.6f", speed);
        setpts = buf;
    }

    std::wstringstream ss;
    ss << L"\"" << ffmpeg << L"\""
       << L" -hide_banner -loglevel warning"
       << L" -y"
       << L" -f gdigrab -framerate " << fps;

    if (desktop || title.empty()) {
        ss << L" -i desktop";
    } else {
        ss << L" -i \"title=" << escape_for_cmdline(title) << L"\"";
    }

    if (audio) {
        // dshow virtual-audio-capturer is the easiest cross-system option, but it
        // is not always installed. We just attempt the default WASAPI loopback if
        // available; otherwise this branch will fail at ffmpeg startup and we
        // surface the error in the log.
        ss << L" -f dshow -i audio=\"virtual-audio-capturer\"";
    }

    ss << L" -vf \"" << setpts << L"\"";

    if (audio) {
        ss << L" -af \"" << build_atempo_chain(speed) << L"\"";
    }

    ss << L" -c:v libx264 -preset veryfast -pix_fmt yuv420p -crf 20";
    if (audio) {
        ss << L" -c:a aac -b:a 160k";
    }

    ss << L" \"" << output_path << L"\"";

    return ss.str();
}

} // anonymous namespace

bool recorder_start(double snapshot_speed) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_active.load()) {
        g_last_error = L"recorder already running";
        return false;
    }

    std::wstring out_dir;
    {
        std::lock_guard<std::mutex> clk(state().cfg_mu);
        out_dir = state().output_dir;
    }
    if (out_dir.empty()) out_dir = default_output_dir();
    if (!ensure_directory(out_dir)) {
        g_last_error = L"failed to create output directory";
        log_wline(L"recorder: cannot create %ls", out_dir.c_str());
        return false;
    }

    std::wstring out_path = timestamped_filename(out_dir);
    std::wstring cmd      = build_command(snapshot_speed, out_path);

    log_wline(L"recorder: cmd = %ls", cmd.c_str());

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_r = nullptr, stdin_w = nullptr;
    if (!CreatePipe(&stdin_r, &stdin_w, &sa, 0)) {
        g_last_error = L"CreatePipe failed";
        return false;
    }
    SetHandleInformation(stdin_w, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_r;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    std::vector<wchar_t> cmd_mut(cmd.begin(), cmd.end());
    cmd_mut.push_back(0);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr, cmd_mut.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr,
        &si, &pi);

    CloseHandle(stdin_r);

    if (!ok) {
        DWORD err = GetLastError();
        CloseHandle(stdin_w);
        wchar_t buf[128];
        swprintf_s(buf, L"CreateProcess failed (err=%lu)", err);
        g_last_error = buf;
        log_wline(L"recorder: %ls", g_last_error.c_str());
        return false;
    }

    g_pi          = pi;
    g_stdin_w     = stdin_w;
    g_last_output = out_path;
    g_last_error.clear();
    g_active.store(true);
    state().recording.store(true);

    log_wline(L"recorder: started -> %ls (snapshot speed=%.4f)",
              out_path.c_str(), snapshot_speed);
    return true;
}

void recorder_stop() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_active.load()) return;

    if (g_stdin_w) {
        const char quit[] = "q\n";
        DWORD written = 0;
        WriteFile(g_stdin_w, quit, (DWORD)(sizeof(quit) - 1), &written, nullptr);
        CloseHandle(g_stdin_w);
        g_stdin_w = nullptr;
    }

    if (g_pi.hProcess) {
        DWORD waited = WaitForSingleObject(g_pi.hProcess, 5000);
        if (waited == WAIT_TIMEOUT) {
            log_line("recorder: ffmpeg did not exit on 'q', terminating");
            TerminateProcess(g_pi.hProcess, 1);
            WaitForSingleObject(g_pi.hProcess, 1000);
        }
        CloseHandle(g_pi.hProcess);
        CloseHandle(g_pi.hThread);
        g_pi = {};
    }

    g_active.store(false);
    state().recording.store(false);
    log_wline(L"recorder: stopped -> %ls", g_last_output.c_str());
}

bool recorder_is_active() {
    return g_active.load();
}

std::wstring recorder_last_output_path() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_last_output;
}

std::wstring recorder_last_error() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_last_error;
}

} // namespace tasdll
