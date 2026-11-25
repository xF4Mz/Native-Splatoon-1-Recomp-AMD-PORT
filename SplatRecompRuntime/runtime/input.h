// Controllers: a PC game controller and keyboard/mouse act as the Wii U
// GamePad (see input.c).
#ifndef INPUT_H
#define INPUT_H

#include "hle.h"
#include <stdbool.h>

// Read the controller, keyboard and mouse (window thread; window may be NULL).
void input_poll(void* glfw_window);

// The GamePad's A button is held right now.
bool input_gamepad_a_pressed(void);
// Is ZR (shoot) held right now?
bool input_zr_held(void);
// Is ZL (squid form) held right now?
bool input_zl_held(void);

const HleEntry* input_hle_table(size_t* count);

#endif
