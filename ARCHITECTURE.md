# Architecture

**The idea.** The Wii U game's PowerPC code is translated to C once (DolRecomp, `dolrecomp.exe --cpu espresso`), then compiled together with a runtime that stands in for the console: the Wii U system libraries are re-implemented on the PC side (HLE), the GPU command stream (GX2 / Latte PM4) is translated to Vulkan, sound to WASAPI. Nothing is interpreted at run time: the game's code runs as native x86-64.

**Folders**
- `SplatRecompRuntime/runtime/` the runtime (C11): `main.c` (startup, threads, the recompiled-code dispatcher), `hle.c` + `sys.c`/`fs.c`/`audio.c`/`gx2.c`/`guest_threads.c`/`erreula.c`/`online.c` (the console's libraries), `gx2_cmd.c` (the game's command ring), `latte_cp.c` (the GPU command thread), `latte_render.c` (Vulkan: buffers, textures, pipelines, present, the settings panel), `latte_shader.c` (shader translation to GLSL → SPIR-V via glslang), `latte_texture.c` (tiled texture layouts), `input.c`, `display.c` (the window), `native_funcs.c` (hand-written fast versions of a few game functions, hooked by address at configure time).
- `SplatRecompRuntime/tools/` `rpx_patch.c` (resolves the RPX's imports before translation), `chunk_split.c` (splits the generated code into optimizable functions), the build script.
- `SplatRecompRuntime/third_party/` GLFW, volk, Vulkan headers, glslang (bundled, their own licenses).
- `SplatRecompRuntime/pgo/splat.profdata` a profile of the game's hot paths, used by Clang when compiling the game code.
- `DolRecomp/` the translator (GPL-3.0): source and a built `dolrecomp.exe`.
- `launcher/` NSR: `nsr.c` (the installer / launcher window: Win32 + GDI+, four steps, settings, music; compiled INTO the game program through `SplatRecompRuntime/runtime/nsr_wizard.c`), `nsr_theme.h` (colors, assets), `nsr_wud.c` (the .wux/.wud reader, in the program through `runtime/nsr_wud_lib.c`; also `build_wud.cmd` -> a standalone `nsr_wud.exe` for `nsr_wud info`), `make_release.py` (the zips). `tools/rpx_patch.c` is in the program too (`runtime/nsr_rpx_lib.c`).

**The install (install_thread in nsr.c, all inside NSR.exe, nothing compiled):** find the base game and the v272 update by their `meta/meta.xml` (or read the base game out of a disc image with the player's keys.txt into `<install>/gamedata/base` and junction the update folder beside it); check the update's `code/Gambit.rpx` by SHA-256; `rpx_patch` -> `data/Gambit_v272_patched.rpx`; write `data/install.ini`; copy NSR.exe into the install folder. Play then runs the game in the same process with the working directory `<install>/game` (shader cache) and the log in `<install>/logs/session_1.txt`. NSR.exe is `recomp_runner.exe` (the runtime + the recompiled game code) renamed.

**At run time** the game reads its content straight from the dump folder (`--game`), saves go to `saves/`, the shader/pipeline caches to `game/cache/`, the log of each session to `logs/session_N.txt` (three kept).

**What's deliberately not here:** game files, keys, generated code, Nintendo art. Disc images (.wux/.wud) are read with the player's own keys.txt (next to the image, Cemu's, or picked in step 1); NSR ships no keys and never downloads any.
