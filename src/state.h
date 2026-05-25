#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace tasdll {

struct State {
    std::atomic<double> speed{1.0};
    std::atomic<bool>   recording{false};
    std::atomic<bool>   gui_visible{true};
    std::atomic<bool>   exit_requested{false};

    std::mutex          cfg_mu;
    std::wstring        output_dir;
    std::wstring        window_title;
    std::wstring        ffmpeg_path;
    int                 capture_fps{60};
    bool                capture_audio{false};
    bool                use_desktop{false};
};

State& state();

std::wstring default_output_dir();
std::wstring foreground_window_title();

} // namespace tasdll
