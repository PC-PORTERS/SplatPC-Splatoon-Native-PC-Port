#pragma once
#include "gx2_vk.h"

// Online play: scales the TV picture (Player 2's screen in Battle Dojo) to the
// stream size on the GPU and hands it to the host encoder once the copy has
// completed, without waiting for the GPU. Call outside a rendering instance.
void gx2_net_capture(gx2vk::Image& tv, uint32_t width, uint32_t height);
