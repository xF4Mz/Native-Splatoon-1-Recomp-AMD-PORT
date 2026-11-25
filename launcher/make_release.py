# Makes the two release zips in releases\ (in the project folder), lists what went in and scans for things
# that must not: game files, keys, generated/compiled game code, personal paths. Run with the project
# root as the working directory:  python launcher\make_release.py v0.23-dev
import os, re, sys, zipfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # (the project folder: launcher\..)
VERSION = sys.argv[1] if len(sys.argv) > 1 else 'v0.23-dev'
OUT = os.path.join(ROOT, 'releases')
os.makedirs(OUT, exist_ok=True)

# never in any zip
BAD_NAMES = re.compile(r'\.(rpx|rpl|wux|wud|wua|iso|szs|sarc|bfres|bfstm|bfsar|splatcap|profraw|obj|pdb|ilk|zip|log|dmp|png|jpg|bin)$|^(keys\.txt|otp\.bin|seeprom\.bin|settings\.ini|launcher\.ini|install\.ini|splatoon_log.*|imports_todo\.txt|debug_shaders\.txt|built_from\.txt|cfg_log\.txt|.*_log\.txt|.*_log_prev.*|splat_settings.*)$', re.I)   # (any *_log.txt: logs hold this PC's paths; splat_settings*: this PC's settings)
BAD_FILES = {'SplatRecompRuntime/tools/go.cmd', 'SplatRecompRuntime/tools/launcher.c', 'SplatRecompRuntime/tools/copy_launcher.cmake'}   # the old zip launcher and a script with this PC's paths
BAD_DIRS = {'emotes', 'build', 'build_ninja', 'build_lto', 'build_pgogen', 'build_splatoon', 'cache', 'debug_output', 'images', 'generated', 'chunks', 'recomp', '.git', 'tmp', 'temp', 'logs', 'saves', 'saves_test', 'saves_backup_2026-10-16', 'backups', 'tests', 'benchmarks', '__pycache__', 'content', 'code', 'meta'}
ALLOW_BIN = set()   # (no binaries in the source zip; the installer zip is NSR.exe itself, added below)
OWN_ART = {'ui/assets/nsr_logo.png'}   # the project's own logo (not game art): allowed in the installer zip
NSR_EXE = r'SplatRecompRuntime\build_ninja\recomp_runner.exe'   # the game program with the installer window built in
PGO = r'SplatRecompRuntime\pgo\splat.profdata'   # profile data (no game content), installer only

def walk(rel_dir):
    for dp, dns, fns in os.walk(os.path.join(ROOT, rel_dir)):
        dns[:] = [d for d in dns if d not in BAD_DIRS]
        for f in fns:
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, ROOT)
            yield rel, full

def wanted(rel, installer):
    base = os.path.basename(rel)
    if rel.replace(chr(92), '/') in BAD_FILES: return False
    if rel in ALLOW_BIN: return installer
    if rel == PGO: return installer
    if rel.replace(chr(92), '/').startswith('SplatRecompRuntime/pgo/'): return False   # (older profiles)
    if BAD_NAMES.search(base): return False
    if base.endswith(('.exe', '.lib', '.dll', '.a')): return False
    return True

def add_tree(z, rel_dir, installer, arc_prefix=''):
    n = 0
    for rel, full in walk(rel_dir):
        if not wanted(rel, installer): continue
        z.write(full, arc_prefix + rel)
        n += 1
    return n

README_INSTALLER = f"""NSR - Native Splat Recomp Loader  ({VERSION})   DEV BUILD - UNSTABLE, EXPECT CRASHES

FOR DEVELOPERS ONLY. This is a development build for testing and working on the port. It is NOT meant to be
played through: expect crashes, bugs and missing features, and don't use your main save.

What this is
  One program, NSR.exe: the installer, the launcher and the native PC version of Splatoon (Wii U v2.7.2) in one.
  Like Unleashed Recomp: you point it at YOUR OWN copy of the game, it prepares it, you play. Nothing to compile,
  no Visual Studio, no downloads. No game files, keys or Nintendo art are in this zip.

You need
  - Your own Dumpling dump of Splatoon: the base game AND the v272 update, each a folder with code\\, content\\, meta\\.
    Or a .wux/.wud disc image of the game with YOUR OWN keys.txt next to it (one key per line) plus a dump of the v272
    update. NSR ships no keys and never downloads any.
  - An AMD Radeon graphics card. THIS IS AN AMD BUILD: it was built and tested on AMD (RX 9060 XT) only.
    NVIDIA and Intel graphics are UNTESTED -- they may show wrong pictures, crash, or not start. If you try one,
    send the log (logs\\session_1.txt) with your card's name.
  - Windows 10/11 x64, a graphics driver with Vulkan, and the Microsoft Edge WebView2 Runtime (built into Windows 11
    and current Windows 10; the launcher window uses it).
  - About 2 GB free (6 GB with a disc image, which is read out into the install folder).

What PC it needs (measured, Oct 19)
  - For a steady 60 fps everywhere, including the busiest plaza view: a CPU with 4 real cores (any Core i3/i5 or
    Ryzen 3/5 from the last few years), 8 GB RAM, a graphics card or chip with a Vulkan 1.3 driver.
    The game uses about 2 GB of RAM in the plaza and 3.5 GB in story levels.
  - A 2-core laptop (4 threads) holds about 50-55 fps in the busiest plaza view and 60 elsewhere. Turn on
    "Performance mode" in the launcher's Settings menu (or in the game's SETTINGS panel): 1x render scale,
    no anti-aliasing, and the plaza runs at the game's own 30 fps -- smooth instead of dipping. Battles and
    story levels still run at 60.
  - Integrated graphics (AMD Vega / Radeon 680M and up) at 1280x720 (render scale 1x) are expected to work;
    not measured yet -- send a log. Intel and NVIDIA: untested (see above).
  - The first visit to each area builds its graphics (the picture can freeze for seconds); only the first time.
  - Not supported: Chromebooks (no Windows, most have no Vulkan 1.3 or an ARM CPU), 32-bit Windows, macOS.

How to install
  1. Extract the WHOLE zip anywhere (NSR.exe needs the ui\\ folder next to it).
  2. Run NSR.exe. Game source: Browse... > Dump folder (or Disc image for a .wux/.wud). Install location: Browse...
     (an empty folder). Then Install (seconds for a dump; a few minutes for a disc image).
  3. Press Play. The launcher closes and the game starts. The first minutes stutter while shaders are built.
     Next time, start NSR.exe (the copy in the install folder works too) and press Play.

Mods
  The Mods tab lists the folders in the install folder's mods\\ (Add mod... copies one in). A mod is a Cemu graphic
  pack (rules.txt) or any folder with a content\\ folder: enabled mods' content files replace the game's. Code patches,
  shader packs and resolution packs aren't supported yet (resolution: F10 in the game).

Where things go
  Only inside the install folder: game\\ (shader cache), data\\ (the prepared game program), saves\\,
  logs\\ (the last 3 sessions and install.txt), mods\\, ui\\, settings.ini and a copy of NSR.exe. Next to NSR.exe
  where you extracted it, NSR writes settings.ini and webview\\ (the launcher window's cache and launcher_log.txt).
  No registry entries, nothing in Documents or on the Desktop.
  Your dump is only read, never changed. "Remove installation" deletes the install folder's content.

Reporting a bug
  Say what you did, attach the newest file from the install folder's logs\\ (logs\\session_1.txt) and a screenshot.
  F12 in the game saves a screenshot into game\\images\\. F9 writes a diagnostic dump into the log if the game freezes.

Controls
  Xbox controller = GamePad. Keyboard/mouse also work. View or M shows the GamePad screen; Menu (+) pauses in story mode
  (that menu lives on the GamePad screen: press View to see it). F10 settings, F11 fullscreen, F3 fps counter.

Credits
  Game code recompiled with DolRecomp by ExpansionPak (lead developer Hyperway; contributors jw, flaffy,
  smallington, Marco Frank, Douglas Whittingham): https://github.com/ExpansionPak/DolRecomp -- thank you!
  DolRecomp turns the game's PowerPC code into C; the runtime, renderer and launcher are this project's own.
  Come say hi on the ExpansionPak Discord.
  AI-assisted development.
  Built into NSR.exe: GLFW, volk, Vulkan-Headers, glslang, zlib, the Microsoft WebView2 loader -- their licenses
  are in THIRD_PARTY_LICENSES.txt.
  Built and tested with: LLVM / Clang, Microsoft Visual C++, CMake, Ninja, the Khronos Vulkan validation layers.
  Cemu was used as a reference for how the console behaves (no Cemu code is in this project). Dumps are made with
  Dumpling.

License
  GPL-3.0 (see LICENSE.txt), the same license as DolRecomp. Splatoon itself belongs to Nintendo: this zip has none
  of its files, and you need your own copy of the game.
"""

# The libraries built into NSR.exe, each with its own license file (their licenses ask for the notice to ship with the program)
THIRD_PARTY = [
    ('GLFW (window and input)', r'SplatRecompRuntime\third_party\glfw\LICENSE.md'),
    ('volk (Vulkan loader)', r'SplatRecompRuntime\third_party\volk\LICENSE.md'),
    ('Vulkan-Headers (Khronos)', r'SplatRecompRuntime\third_party\vulkan-headers\LICENSE.md'),
    ('glslang (Khronos shader compiler)', r'SplatRecompRuntime\third_party\glslang\LICENSE.txt'),
    ('zlib (compression)', r'zlib\LICENSE'),
    ('Microsoft WebView2 loader (launcher window)', r'launcher\third_party\webview2\LICENSE.txt'),
]
def third_party_licenses():
    out = ['Libraries built into NSR.exe, and their licenses.\n'
           'The game code is recompiled with DolRecomp by ExpansionPak (GPL-3.0, see LICENSE.txt).\n']
    for title, rel in THIRD_PARTY:
        out.append('\n' + '=' * 100 + '\n' + title + '\n' + '=' * 100 + '\n')
        out.append(open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace').read())
    return ''.join(out)

def scan(zpath):
    hexkey = re.compile(r'\b[0-9a-fA-F]{32}\b')
    hits = []
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            name = info.filename
            low = name.lower()
            if BAD_NAMES.search(os.path.basename(low)) and not any(low.endswith(x) for x in ('.lib',)) and low not in OWN_ART:
                hits.append(('forbidden name', name))
            if low.endswith('.exe') and name != 'NSR.exe': hits.append(('program other than NSR.exe', name))
            if 'third_party/' in low: continue   # (bundled libraries: GLFW's gamepad GUID table is all 32-hex strings)
            if low.endswith(('.c', '.h', '.txt', '.md', '.cmd', '.cmake', '.py', '.ini', '.json', '.yml')) or 'cmakelists' in low:
                try: text = z.read(name).decode('utf-8', 'replace')
                except Exception: continue
                for m in hexkey.finditer(text):
                    ctx = text[max(0, m.start() - 40): m.end() + 10].replace('\n', ' ')
                    hits.append(('32-hex string (key-like?)', f'{name}: ...{ctx}...'))
                private = [w for w in os.environ.get('NSR_PRIVATE_WORDS', '').split(';') if w]   # (your own user name / folder names, ';'-separated)
                for word in ['C:\\Users\\'] + private:   # (the word keys.txt is fine: it tells players where THEIR file goes; a file named keys.txt is still refused by BAD_NAMES)
                    if word in text: hits.append((f'personal path / word "{word}"', name))
    return hits

def build(installer):
    name = f'NSR-{"Installer" if installer else "Source"}-{VERSION}.zip'
    path = os.path.join(OUT, name)
    if os.path.exists(path): os.remove(path)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        n = 0
        if installer:
            z.writestr('README.txt', README_INSTALLER); n += 1
            z.write(os.path.join(ROOT, 'DolRecomp', 'LICENSE'), 'LICENSE.txt'); n += 1   # (GPL-3.0, as DolRecomp)
            z.writestr('THIRD_PARTY_LICENSES.txt', third_party_licenses()); n += 1      # (the libraries inside NSR.exe)
            z.write(os.path.join(ROOT, NSR_EXE), 'NSR.exe'); n += 1
            # the launcher's page and its files (the menu song is xF4Mz's own track, not game music)
            for dp, dns, fns in os.walk(os.path.join(ROOT, 'launcher', 'ui')):
                for f in fns:
                    full = os.path.join(dp, f)
                    z.write(full, 'ui/' + os.path.relpath(full, os.path.join(ROOT, 'launcher', 'ui')).replace(chr(92), '/')); n += 1
            z.writestr('ui/assets/README.txt', 'nsr_logo.png / nsr_logo.ico: the NSR logo (our own art, no Nintendo art). music/menu.mp3 is the launcher song.\n'); n += 1
        else:
            n += add_tree(z, 'SplatRecompRuntime', installer)
            n += add_tree(z, 'DolRecomp', installer)
            n += add_tree(z, 'launcher', installer)
            z.writestr('README.md', open(os.path.join(ROOT, 'launcher', 'REPO_README.md'), encoding='utf-8').read()); n += 1
            z.write(os.path.join(ROOT, 'DolRecomp', 'LICENSE'), 'LICENSE'); n += 1   # (GPL-3.0, as DolRecomp)
            z.writestr('ARCHITECTURE.md', open(os.path.join(ROOT, 'launcher', 'ARCHITECTURE.md'), encoding='utf-8').read() if os.path.exists(os.path.join(ROOT, 'launcher', 'ARCHITECTURE.md')) else ''); n += 1
            z.writestr('.gitignore', open(os.path.join(ROOT, 'SplatRecompRuntime', '.gitignore'), encoding='utf-8').read() + 'releases/\nlauncher/build/\n'); n += 1
    print(f'== {name}: {n} files, {os.path.getsize(path) / 1048576:.1f} MB')
    with zipfile.ZipFile(path) as z:
        total = 0
        with open(path + '.files.txt', 'w', encoding='utf-8') as lst:
            for info in sorted(z.infolist(), key=lambda i: i.filename):
                lst.write(f'{info.file_size:>12,}  {info.filename}\n'); total += info.file_size
            lst.write(f'{total:>12,}  TOTAL (uncompressed)\n')
        print(f'   listed in {os.path.basename(path)}.files.txt, {total / 1048576:.1f} MB uncompressed')
    hits = scan(path)
    seen = set()
    print(f'   scan: {len(hits)} findings')
    for kind, what in hits:
        key = (kind, what[:200])
        if key in seen: continue
        seen.add(key)
        print(f'     - {kind}: {what[:200]}')

build(True)
build(False)
