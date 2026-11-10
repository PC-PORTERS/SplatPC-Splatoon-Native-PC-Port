// online_boot.h - what the game does itself in online mode on Windows, so playing online needs no Python:
// the SpotPass files (Game/boss) and the friends relay with its integrity proof (RBREW_SOCIAL_URL, set by
// Launch-Online.ps1). Linux keeps the Python tools started by splatoon_login.py.
#pragma once

namespace rbrew_native {

void online_boot();     // at start-up, before the game reads SpotPass data; does nothing offline
void online_shutdown(); // stops the relay thread (it never blocks the exit)

} // namespace rbrew_native
