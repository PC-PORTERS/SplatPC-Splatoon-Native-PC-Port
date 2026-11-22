#pragma once
// Two-player views: Story Mode co-op (View 0 TV = P1 host, View 1 GamePad = P2
// guest) and Battle Dojo (View 0 TV = P2 guest, View 1 GamePad = P1 host).
#include <cstdint>

namespace rbrew_coop {

void update_views(uint8_t* mem); // VPADRead, every frame
bool two_views();                // in a two-view story level now
bool dojo_match();               // in a Battle Dojo match now
bool second_player_present();    // netplay guest connected or P2 controller attached

} // namespace rbrew_coop
