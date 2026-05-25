#pragma once

namespace tasdll {

bool speedhack_install();
void speedhack_uninstall();

// Reanchors fake clocks on each call so there is no time jump.
void speedhack_set_speed(double speed);

double speedhack_current_speed();

} // namespace tasdll
