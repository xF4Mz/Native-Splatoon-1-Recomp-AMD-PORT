// The game's view of files: /vol/content, /vol/code, /vol/meta and saves.
//
// /vol/content (and code, meta) are read from the player's own game
// folders, update first, then the base game, the same way the console
// layers an update over the disc. /vol/save goes to a "saves" folder on
// the PC.
#ifndef FS_H
#define FS_H

#include "hle.h"

// game_dir: a folder containing the game (and optionally its update), each
// as code/content/meta folders. It's searched a few levels deep.
// saves_dir: where save data lives (created if needed).
// mlc_dir: optional Cemu-style mlc01 folder for system files (may be NULL).
bool fs_setup(const char* game_dir, const char* saves_dir, const char* mlc_dir);

const HleEntry* fs_hle_table(size_t* count);

#endif
