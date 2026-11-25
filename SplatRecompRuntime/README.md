# SplatRecomp runtime and graphics engine (early alpha)

*Burned a bridge. Build a boat.*

The graphics engine is the name of the engine half of this project: the Wii U runtime and the
GX2-to-Vulkan renderer that Splatoon runs on. It is written to carry other Wii U ports
later; ENGINE.md says which files are the engine and which are Splatoon.

The runtime half of a static recompilation of Wii U software. DolRecomp
translates a game's PowerPC code into C; this runtime loads the game's
.rpx, provides Wii U memory, and answers the game's calls into the Wii U
operating system with PC implementations, so the translated code can run
natively.

**Status (v0.9.0):** first 3D renderer. GX2 calls become GPU command
packets like on the console; a GPU thread reads them, translates the game's
GPU shader programs to GLSL/SPIR-V, and draws with Vulkan 1.3. Tested with
small PowerPC programs (textured, depth-tested, indexed drawing). Window
keys: F1 = texture gallery, F12 = screenshot (frame_XXXX.png).

This project contains no game code or game data. You need your own copy of
any game you use it with.

## Layout

- `runtime/main.c`: loads the program and runs it.
- `runtime/rpx_loader.c`: unpacks an .rpx into guest memory, finds imports.
- `runtime/wiiu_mem.c`: the Wii U memory map. The whole 4 GB guest address
  space is one block of PC memory (pages are only used once touched).
- `runtime/fast_cpu.h`: makes the generated code read and write guest
  memory directly (wiiu_mem base + address, byte-swapped) instead of going
  through DolRecomp's checked path. Selected with DOLRECOMP_CPU_HEADER, so no
  re-run of DolRecomp is needed. `--watch` switches back to the checked path.
- `runtime/profiler.c`: samples where the game spends its time and prints
  a summary (fps, game code vs system calls vs idle, busiest functions)
  when the game exits.
- `runtime/hle.c`: PC versions of Wii U system functions (coreinit),
  including the memory heaps the game builds itself (each keeps its own
  accounting, since the game decides things from how much room is left).
- `runtime/hle_format.c`: printf-style formatting for calls like OSReport.
- `runtime/guest_threads.c`: Wii U threads, locks, events, message queues,
  alarms. Each game thread runs on its own PC thread; only one runs at a
  time, handing over at blocking calls, every 8192 jumps, and as soon as a
  thread is caught busy-waiting (a loop coming back in exactly the same
  state), so worker threads get to finish the jobs it is waiting for.
- `runtime/host_thread.h`: small Windows/pthreads wrapper.
- `runtime/fs.c`: the game's files. `/vol/content`, `/vol/code` and
  `/vol/meta` come from the player's game folder (update first, then base
  game, found by reading each folder's `meta/meta.xml`); `/vol/save` goes to
  a saves folder on the PC.
- `runtime/sys.c`: calendar time, locked cache, ProcUI, system settings,
  user account basics, the console's built-in font (from `fonts/` or the PC).
- `runtime/hostfs.c`: portable PC file helpers.
- `runtime/input.c`: a PC game controller (Xbox etc.), keyboard and mouse act
  as the GamePad. Buttons go by position (Xbox A = GamePad B, Xbox B = A).
  The View button (or M) shows the GamePad's own screen over the game; while
  it's up the right stick moves a cursor and A (or the mouse) touches it.
- `runtime/online.c`: accounts, internet, friends, amiibo and sockets,
  answered like an offline console.
- `runtime/erreula.c`: the system error viewer; messages are printed to the
  log and answered automatically (or with the GamePad's A button).
- `runtime/zlib125.c`: the console's zlib, done with the PC's zlib.
- `runtime/gx2.c`, `runtime/gx2_cmd.c`: the graphics library. Like the
  console's GX2, calls write GPU command packets (PM4) into a command stream
  or into display lists in game memory; a few of our own packets cover
  clears, copies and presenting.
- `runtime/latte_cp.c`: the GPU thread. Reads the command stream and display
  lists, keeps the GPU registers (including GX2 context-state shadowing) and
  hands draws to the renderer. Retires fences so GX2DrawDone etc. return.
- `runtime/latte_shader.c`: translates Latte (R700) shader programs - control
  flow, ALU groups, texture and vertex fetches, exports - to GLSL.
- `runtime/glsl_compile.c`: GLSL -> SPIR-V with the bundled glslang.
- `runtime/latte_render.c`: the Vulkan renderer: color/depth buffers kept on
  the graphics card, textures untiled and uploaded from game memory
  (`latte_texture.c`), pipelines, draws, clears, copy to the TV picture and
  presenting it in the window. `--dump-frames N` saves the first N frames.
- `runtime/vk_ctx.c`: Vulkan device setup, shared by the window and GPU threads.
- `runtime/display.c`: the game window (GLFW), on its own PC thread: keeps it
  responsive, reads input, shows fps in the title. `--no-window` turns it off.
- `runtime/gx2_texture.c`: reads the game's textures from guest memory:
  undoes the GPU's tiled layout (1D and 2D thin tiling, checked against
  AMD's R600 address math) and decodes BC1-BC5 and common 8/16/32-bit formats.
- `runtime/texture_gallery.c`: F1 shows every texture the game binds
  (GX2SetPixelTexture etc.), newest first.
- `runtime/cpu_fallback.c`: instructions the recompiler leaves to the
  runtime (cache operations, special registers).
- `tools/rpx_patch.c`: fills in an .rpx's import addresses before
  recompiling (Nintendo's tools leave them for the console's loader).
- `tests/threads/`, `tests/files/`: tiny PowerPC test programs for the
  thread and file code (build with clang; see each `build.sh`).

## Building (Windows, Developer Command Prompt)

1. Recompile the program with DolRecomp:
   `dolrecomp.exe --cpu espresso program.rpx C:\recomp`
2. Build the runner against that output:
   ```
   cmake -S . -B build -DGENERATED_DIR=C:/recomp/generated -DZLIB_ROOT=<your zlib folder> -DZLIB_USE_STATIC_LIBS=ON
   cmake --build build --config Release
   ```
3. Run it: `build\Release\recomp_runner.exe F:\wiiutest\wiiutest.rpx`

For a game, also tell the runner where its files are:
`recomp_runner.exe Game_patched.rpx --game "<folder holding the game and its update>" --saves "<saves folder>"`

## How a system call works

Wii U programs call OS functions indirectly, by jumping to an import address
in the 0xC0000000 range. DolRecomp's generated code offers a `host_call`
hook before every jump; the runtime checks whether the target is an import,
runs the matching function in `hle.c`, puts the result in r3 and returns to
the caller. Unimplemented imports are logged as `[hle] TODO name` and return
0, which makes the next function to implement easy to find.

## License

GPL-3.0-or-later, matching DolRecomp.

Bundled in `third_party/` under their own licenses (each folder has its
license file): GLFW (zlib), volk (MIT), Vulkan-Headers (Apache-2.0),
glslang (BSD-3-Clause / Apache-2.0, see `third_party/glslang/LICENSE.txt`).
