// Debugging aid: find code that reads or writes a given address.
#ifndef SCAN_H
#define SCAN_H

#include "rpx_loader.h"

// Prints every store/load in the game's code that touches `address`, with
// the code of the function doing the store and its direct callers.
void scan_for_address(const RpxImage* image, u32 address);

// Prints the tree of functions that call `function` (several levels up),
// including tables in data that hold its address.
void scan_callers(const RpxImage* image, u32 function);

// Find a piece of text in the game's data and print the code that uses it.
void scan_find_text(const RpxImage* image, const char* text);

// Start of the function containing `at` (best guess from the code around
// it; `sec_start` is the start of its code section).
u32 scan_function_start(u32 at, u32 sec_start);

#endif
