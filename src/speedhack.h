#pragma once

#include <cstdint>
#include <string>

namespace tasdll {

bool speedhack_install();
void speedhack_uninstall();

void   speedhack_set_speed(double speed);
double speedhack_current_speed();

// Returns a formatted multi-line string with per-hook hit counts and install
// status, so the user can verify the hooks are being called by the game.
std::string speedhack_stats();

} // namespace tasdll
