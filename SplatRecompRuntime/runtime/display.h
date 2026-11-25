// The game window: a PC window with a Vulkan swapchain, run on its own
// thread. For now it shows a color that shifts with every frame the game
// finishes (so you can see it's alive) and the frame rate in the title.
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

// Open the window (unless headless). Never fails the game: if the window or
// Vulkan can't start, the reason is printed and the game runs without one.
void display_start(bool headless);

// The game finished a frame (GX2SwapScanBuffers).
void display_frame_done(void);

#endif
