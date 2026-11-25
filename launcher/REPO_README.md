# Native Splatoon 1 Recomp (AMD port)

A native PC version of **Splatoon** (Wii U, v2.7.2). The game's PowerPC code is recompiled to C ahead of time and runs
as a normal Windows program, with Vulkan graphics, WASAPI sound and Xbox controller / keyboard / mouse input.

> **For developers only.** This is a development build for testing and working on the port. It is **not** meant to
> be played through: expect crashes, bugs and missing features, and don't use your main save.
> **AMD Radeon only:** built and tested on an RX 9060 XT. NVIDIA and Intel graphics are untested and may not work.

## What you need

- Your **own** dump of Splatoon: the base game **and** the v272 update (Dumpling folders with `code`, `content`,
  `meta`), or a `.wux` / `.wud` disc image with your own `keys.txt`. This repository has **no game files, no keys and
  no recompiled game code**, and never downloads any.
- Windows 10 / 11 (64-bit), an AMD Radeon card with a Vulkan 1.3 driver, the WebView2 runtime (built into Windows 11).

## Playing

Download the installer zip from **Releases**, extract all of it, run `NSR.exe`, pick your dump and an empty folder,
press **Install**, then **Play**. The README inside the zip has the details, controls and how to report a bug.

## What's in here

| Folder | What it is |
|---|---|
| `SplatRecompRuntime/` | the runtime: the Wii U system libraries re-implemented for PC, the GX2 / Latte GPU translated to Vulkan, sound, input |
| `launcher/` | NSR, the installer / launcher window (Win32 + WebView2) |
| `DolRecomp/` | the recompiler (GPL-3.0), included as used |

`ARCHITECTURE.md` explains how the parts fit together. Building `NSR.exe` needs DolRecomp's output for your own
v272 `Gambit.rpx`; nothing game-derived is in this repository.

## How to build

These are the steps this project is built with. They haven't been tried on a fresh PC yet, so expect to fix a path
or two. **Never commit or share the recompiled game code** (step 3's output): it is Nintendo's code in another form.

**You need:** Windows 10/11 x64; Visual Studio 2022 with *Desktop development with C++* **and** *C++ Clang tools for
Windows* (the game code is compiled with clang-cl); CMake 3.16+ and Ninja (both come with Visual Studio); a static
zlib (for example `vcpkg install zlib:x64-windows-static`); your own Splatoon dump with the v272 update.
Run everything from the **x64 Native Tools Command Prompt for VS 2022**.

1. **Build the recompiler.**
   ```
   cmake -S DolRecomp -B DolRecomp/build
   cmake --build DolRecomp/build --config Release
   ```
2. **Get the patched game program.** The v272 update's `code/Gambit.rpx` needs its import addresses filled in
   first. The easy way: install once with the release's `NSR.exe`; it writes `data\Gambit_v272_patched.rpx` in the
   install folder. (Or build `SplatRecompRuntime/tools/rpx_patch.c` and run `rpx_patch Gambit.rpx Gambit_v272_patched.rpx`.)
3. **Recompile it to C** (keep this output to yourself):
   ```
   DolRecomp\build\Release\dolrecomp.exe --cpu espresso -j8 Gambit_v272_patched.rpx C:\splat_recomp
   ```
   This writes `C:\splat_recomp\generated` (with `generated.h` and a `chunks` folder).
4. **Build the program.**
   ```
   cmake -S SplatRecompRuntime -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
     -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DSPLAT_DIRECT_CALLS=ON -DSPLAT_CLANG_ARCH=x86-64-v3 ^
     -DGENERATED_DIR=C:/splat_recomp/generated ^
     -DZLIB_INCLUDE_DIR=<zlib>/include -DZLIB_LIBRARY=<zlib>/lib/zlib.lib
   cmake --build build
   ```
   The first build compiles the whole game and takes a while (15+ minutes). The result is
   `build\recomp_runner.exe`.
5. **Run it.** Either directly:
   ```
   build\recomp_runner.exe Gambit_v272_patched.rpx --game "<your dump folder>" --saves saves
   ```
   or as the launcher: rename it `NSR.exe` and put the `launcher\ui` folder next to it as `ui`.

The release builds also use a profile of the game's hot paths (`-DSPLAT_PGO=use` with a `pgo\splat.profdata` file),
which isn't in this repository: without it the program works the same, a little slower.

## Credits

- **[DolRecomp](https://github.com/ExpansionPak/DolRecomp) by ExpansionPak**: lead developer **Hyperway**, with
  jw, flaffy, smallington, Marco Frank and Douglas Whittingham. DolRecomp recompiles the game's PowerPC code into C.
  Thank you!
  Come say hi on the ExpansionPak Discord.
- The runtime, renderer and launcher: this project (xF4Mz).
- AI-assisted development.
- Built into the program, each under its own license (bundled in `third_party/` folders; the release zip has them all
  in `THIRD_PARTY_LICENSES.txt`): [GLFW](https://www.glfw.org), [volk](https://github.com/zeux/volk),
  [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), [glslang](https://github.com/KhronosGroup/glslang),
  [zlib](https://zlib.net), the Microsoft WebView2 loader.
- Built and tested with: LLVM / Clang, Microsoft Visual C++, CMake, Ninja, the Khronos Vulkan validation layers.
- [Cemu](https://cemu.info) was used as a reference for how the console behaves (no Cemu code is in this project).
  Dumps are made with Dumpling (Wii U homebrew).

## License

GPL-3.0 (see `LICENSE`), the same license as DolRecomp.

Splatoon, Wii U and Nintendo are trademarks of Nintendo. This is an unofficial fan project, not affiliated with or
endorsed by Nintendo.
