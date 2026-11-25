# The graphics engine

*If you burned a bridge, then build a boat.*  (short: **Burned a bridge. Build a boat.**)

The graphics engine is the name of everything in this folder that is not Splatoon: the Wii U runtime
(memory, threads, file system, the Cafe OS calls the game makes) and the renderer (the GX2 command
processor, the Latte shader translator, Vulkan drawing, audio, input, the settings panel). Splatoon
is the first game that runs on it. The point of naming it is to keep the engine reusable for the
next Wii U port, so this file says what belongs to which side and what a second port would need.

It has no brand name (Nov 25, 2026: the old name was dropped). It shows as "graphics engine" in the
settings panel's title bar and the log's `[engine]` line.

## What is engine (reusable as it is, or with a table swap)

| Area | Files | Splatoon-specific bits to lift out |
|---|---|---|
| Program loading | rpx_loader.c, wiiu_mem.c, scan.c | the .rpx name and the patched-RPX check in main.c; scan.c's start-point scan |
| CPU glue | cpu_fast.c, cpu_fallback.c, fast_cpu.h, split_inline.h, fp_fast.h | fast_chunks.txt (the hot-file list is per game) |
| Cafe OS calls | hle.c, hle_format.c, guest_threads.c, sys.c, fs.c, hostfs.c, erreula.c | the save folder layout (`--saves`), a few return-address checks (OSGetTime's caller for deterministic mode) |
| Graphics | gx2.c, gx2_cmd.c, gx2_layout.c, gx2_texture.c, latte_cp.c, latte_shader.c, latte_texture.c, latte_render.c, latte_capture.c, glsl_compile.c, vk_ctx.c, display.c, texture_gallery.c | the plaza-30 request (a game struct bit), the sun-shadow-map recognition, the sky-dome recognition for SHADERS, the ink-map readback heuristics, the wide-screen projection hooks (sead addresses) |
| Sound | audio.c | nothing (AX voices + WASAPI are generic); rain_sound.c is a Splatoon effect |
| Input | input.c | the Pro-Controller player-2 route (Battle Dojo), the Xbox layout labels |
| Diagnostics | profiler.c, frame_tracker.c, nan_trap.c | the return-address lists in the stall / stutter reports |
| Launcher | launcher/nsr.c, nsr_wud.c, ui/ | the game's title ID, the v272 update check, the install wizard's texts |

## What is Splatoon (stays with the game)

native_funcs.c (camera guard, FOV, projection hooks at game addresses), camera_track.c (the game's camera, the player's position), freecam.c,
benchmark.c (map table swap, scene names), online.c / net_offline.c (NEX, offline only), scene_place.c (where the player is)
rain_sound.c, the generated code in
`splatoon_v272\generated\`, pipeline_keys.bin, shader_cache.bin and the `cache\` folder.

## The address problem

Every game-specific thing above is a PowerPC address (02xxxxxx code, 10xxxxxx data) found by hand
for Splatoon v272. Counted Oct 31: about 55 such addresses across main.c (11), nan_trap.c (9),
camera_track.c (1), latte_render.c (5), gx2.c (4), native_funcs.c (4), scan.c (4), hle.c (3) and a few
others. The cleanest second-port path is one `game_table.h` per game holding those addresses and
flags (which bit asks for 30 fps, which cameras are scene cameras, which buffer is the sun shadow
map), with the engine files reading the table instead of literals. Nothing forces that split now;
do it when the second port starts, address by address, and keep the Splatoon build bit-identical
while doing it (the replay captures in `debug_output\` are the check).

## What a second Wii U port needs

1. The game's code as C from DolRecomp (the same way `splatoon_v272\generated\` was made), with
   its own hot-file list for the fast CPU path.
2. Its .rpx imports listed (`imports_todo.txt` style) -- hle.c covers Splatoon's; another game
   will call functions Splatoon never did.
3. A pass over the renderer heuristics in the table above, each tested with a frame capture (F12)
   and a replay.
4. Its own warm-up list (`pipeline_keys.bin`) and shader cache; the cache format is engine.
5. The launcher's wizard texts, title ID and update version.

Rules carried over: the engine contains no game code, data, keys or Nintendo art; Cemu is a
semantics reference only; nothing from RebrewU / Gambit / rebrew is reused.
