#pragma once

#include <string>

namespace tasdll {

// Captures via ffmpeg's gdigrab (or desktop) and applies setpts=PTS*speed so the
// final video shows game-time at 1x even when speedhack is active.
//
// snapshot_speed is captured once at start. Live speed changes during a recording
// are NOT compensated mid-stream; stop and restart to re-snapshot.
bool recorder_start(double snapshot_speed);
void recorder_stop();
bool recorder_is_active();

std::wstring recorder_last_output_path();
std::wstring recorder_last_error();

} // namespace tasdll
