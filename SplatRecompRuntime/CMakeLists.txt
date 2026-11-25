cmake_minimum_required(VERSION 3.20)
project(SplatRecompRuntime C CXX)

# Where things live. Defaults match the folder layout from the setup guide;
# override with -DDOLRECOMP_DIR=... etc. if yours differ.
set(DOLRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../DolRecomp" CACHE PATH "DolRecomp source folder")
set(GENERATED_DIR "" CACHE PATH "Folder DolRecomp wrote (the one containing generated.h)")
set(RUNNER_NAME "recomp_runner" CACHE STRING "Name of the program to build")

if(NOT GENERATED_DIR OR NOT EXISTS "${GENERATED_DIR}/generated.h")
    message(FATAL_ERROR
        "Set GENERATED_DIR to the folder containing generated.h, e.g.\n"
        "  -DGENERATED_DIR=F:/wiiutest/recomp/generated")
endif()
if(NOT EXISTS "${DOLRECOMP_DIR}/src/cpu/cpu.c")
    message(FATAL_ERROR "DOLRECOMP_DIR doesn't look like DolRecomp: ${DOLRECOMP_DIR}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# The window: GLFW (zlib license) makes it, volk (MIT) loads Vulkan from the
# graphics driver at run time, Vulkan-Headers (Apache-2.0) describe the API.
# All three are bundled in third_party/, so nothing extra needs installing.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
add_library(volk STATIC third_party/volk/volk.c)
target_include_directories(volk PUBLIC third_party/volk third_party/vulkan-headers/include)
if(NOT WIN32)
    target_link_libraries(volk PRIVATE ${CMAKE_DL_LIBS})
endif()

# Shaders: glslang (BSD/Apache-2.0, bundled) compiles the GLSL our shader
# translator writes into SPIR-V for Vulkan. Only the library is built.
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glslang EXCLUDE_FROM_ALL)
if(MSVC)
    # compile glslang's files in parallel too (it's only built once)
    target_compile_options(glslang PRIVATE /MP)
endif()

# Game threads run on several PC cores at once (like the Wii U's three), so
# the game's atomic operations (lwarx / stwcx.) must really be atomic. Rewrite
# the few game-code files that use them to call runtime/splat_atomic.h
# instead (once; only those files get rebuilt). Nothing is regenerated.
set(SPLAT_ATOMICS_MARK "${GENERATED_DIR}/splat_atomics_v1.txt")
if(NOT EXISTS "${SPLAT_ATOMICS_MARK}")
    message(STATUS "Making the game's atomic operations safe for several cores (one time, about a minute)...")
    file(GLOB SPLAT_ALL_CHUNKS "${GENERATED_DIR}/chunks/*.c")
    set(SPLAT_PATCHED 0)
    foreach(chunk IN LISTS SPLAT_ALL_CHUNKS)
        file(READ "${chunk}" body)
        string(FIND "${body}" "ctx->reserve_valid = true;" has_lwarx)
        string(FIND "${body}" "bool success = ctx->reserve_valid && ea == ctx->reserve_addr;" has_stwcx)
        if(has_lwarx GREATER -1 OR has_stwcx GREATER -1)
            string(REGEX REPLACE
                "ctx->gpr\\[([0-9]+)\\] = mem_read32\\(ctx, ea\\);[\r\n]+[ ]*ctx->reserve_addr = ea;[\r\n]+[ ]*ctx->reserve_valid = true;"
                "ctx->gpr[\\1] = splat_lwarx(ctx, ea);" body "${body}")
            string(REGEX REPLACE
                "bool success = ctx->reserve_valid && ea == ctx->reserve_addr;[\r\n]+[ ]*if \\(success\\) \\{ mem_write32\\(ctx, ea, ctx->gpr\\[([0-9]+)\\]\\); ctx->reserve_valid = false; \\}"
                "bool success = splat_stwcx(ctx, ea, ctx->gpr[\\1]);" body "${body}")
            string(FIND "${body}" "#include \"splat_atomic.h\"" has_include)
            if(has_include EQUAL -1)
                string(REPLACE "#include \"../generated.h\"" "#include \"../generated.h\"\n#include \"splat_atomic.h\"" body "${body}")
            endif()
            file(WRITE "${chunk}" "${body}")
            math(EXPR SPLAT_PATCHED "${SPLAT_PATCHED} + 1")
        else()
            string(FIND "${body}" "splat_lwarx(" done_before)
            if(done_before GREATER -1)
                math(EXPR SPLAT_PATCHED "${SPLAT_PATCHED} + 1")
            endif()
        endif()
    endforeach()
    message(STATUS "  ${SPLAT_PATCHED} game-code files updated")
    file(WRITE "${SPLAT_ATOMICS_MARK}" "${SPLAT_PATCHED}\n")
endif()
file(READ "${SPLAT_ATOMICS_MARK}" SPLAT_PATCHED)
string(STRIP "${SPLAT_PATCHED}" SPLAT_PATCHED)
if(SPLAT_PATCHED GREATER 0)
    set(SPLAT_ATOMICS_ON 1)
else()
    set(SPLAT_ATOMICS_ON 0)
endif()
# (tells the runtime whether that happened: without it, game threads stay on one core)
set(SPLAT_ATOMICS_SRC "${CMAKE_CURRENT_BINARY_DIR}/atomics_state.c")
set(SPLAT_ATOMICS_TEXT "// written by CMakeLists.txt\nint splat_atomics_patched = ${SPLAT_ATOMICS_ON};\n")
if(EXISTS "${SPLAT_ATOMICS_SRC}")
    file(READ "${SPLAT_ATOMICS_SRC}" SPLAT_ATOMICS_OLD)
else()
    set(SPLAT_ATOMICS_OLD "")
endif()
if(NOT SPLAT_ATOMICS_OLD STREQUAL SPLAT_ATOMICS_TEXT)
    file(WRITE "${SPLAT_ATOMICS_SRC}" "${SPLAT_ATOMICS_TEXT}")
endif()

# Some game functions are done natively (runtime/native_funcs.c): a line
# at the start of each in the game code hands it over. Only the file with
# that function is touched (and rebuilt). Nothing is regenerated.
set(SPLAT_NATIVE_MARK "${GENERATED_DIR}/splat_native_v9.txt")
if(NOT EXISTS "${SPLAT_NATIVE_MARK}")
    set(SPLAT_NATIVE_FUNCS "028FDCE0=splat_native_yaz0" "02D1E130=splat_native_frustum_box"
        "02D1D730=splat_native_sphere_xform" "02D1DA4C=splat_native_box_xform" "02D349B8=splat_native_mtx_copy"
        "02D34910=splat_native_mtx_multvec" "02D34964=splat_native_mtx_multvecsr"
        "0251C518=splat_native_point_in_box" "028E85C4=splat_native_perspective" "02DD03A0=splat_native_mtx_perspective" "028E8AFC=splat_native_ortho" "0265C0FC=splat_native_anim_wrap>0265C108" "028E6CFC=splat_native_lookat" "02D26D80=splat_native_skel_world" "0242B094=splat_native_duel_draw")
    file(GLOB SPLAT_CHUNK_LIST "${GENERATED_DIR}/chunks/chunk_*.c")
    set(SPLAT_NATIVE_DONE 0)
    foreach(entry IN LISTS SPLAT_NATIVE_FUNCS)
        string(REPLACE "=" ";" parts "${entry}")
        list(GET parts 0 faddr)
        list(GET parts 1 fname)
        # "addr=fn>label": when fn returns 1 the game code continues at that label (a loop done natively) instead of returning
        string(REPLACE ">" ";" fparts "${fname}")
        list(LENGTH fparts nparts)
        list(GET fparts 0 fname)
        if(nparts GREATER 1)
            list(GET fparts 1 ftarget)
            set(ftail "goto label_${ftarget};")
        else()
            set(ftail "{ ctx->pc = ctx->lr & ~3u; return; }")
        endif()
        math(EXPR want "0x${faddr}")
        # the chunk starting at or just before that address
        set(best "")
        set(best_start 0)
        foreach(chunk IN LISTS SPLAT_CHUNK_LIST)
            get_filename_component(cname "${chunk}" NAME_WE)
            if(cname MATCHES "_([0-9A-Fa-f]+)$")
                math(EXPR start "0x${CMAKE_MATCH_1}")
                if(NOT start GREATER want AND NOT start LESS best_start)
                    set(best "${chunk}")
                    set(best_start ${start})
                endif()
            endif()
        endforeach()
        if(best)
            file(READ "${best}" body)
            string(FIND "${body}" "label_${faddr}:" at)
            string(FIND "${body}" "${fname}(ctx)" already)
            if(at GREATER -1 AND already EQUAL -1)
                string(REPLACE "label_${faddr}:" "label_${faddr}: if (${fname}(ctx)) ${ftail}" body "${body}")
                string(REPLACE "#include \"../generated.h\"" "#include \"../generated.h\"\nint ${fname}(CPUState* ctx);" body "${body}")
                file(WRITE "${best}" "${body}")
                math(EXPR SPLAT_NATIVE_DONE "${SPLAT_NATIVE_DONE} + 1")
            endif()
        endif()
    endforeach()
    message(STATUS "Native game functions: ${SPLAT_NATIVE_DONE} hooked up")
    file(WRITE "${SPLAT_NATIVE_MARK}" "${SPLAT_NATIVE_DONE}\n")
endif()

file(GLOB GENERATED_CHUNKS CONFIGURE_DEPENDS "${GENERATED_DIR}/chunks/*.c")

# Visual Studio stops optimizing a function once it is very big (warning
# C4883), and DolRecomp writes each game-code file as one very big function
# (16 KB of game code), so all of them were built unoptimized: every value
# through the stack, every memory access a call. tools/chunk_split.c rewrites
# the files that hold the game's busiest code (runtime/fast_chunks.txt, from
# the profiler's hot_chunks.txt) as many small functions, into the build
# folder; those get built fully optimized. The other files stay as they are
# (nothing else rebuilds).
set(SPLAT_FAST_CHUNKS "")   # the split files (build folder)
set(SPLAT_FAST_SRC "")      # their originals
option(SPLAT_SPLIT_ALL "Split every game-code file (for testing)" OFF)
set(SPLAT_FAST_LIST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/fast_chunks.txt")
set(SPLAT_SPLIT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tools/chunk_split.c")
if(EXISTS "${SPLAT_FAST_LIST_FILE}" AND (MSVC OR SPLAT_SPLIT_ALL))
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPLAT_FAST_LIST_FILE}" "${SPLAT_SPLIT_SRC}")
    file(STRINGS "${SPLAT_FAST_LIST_FILE}" SPLAT_FAST_LIST REGEX "^[0-9A-Fa-f]+")
    foreach(chunk IN LISTS GENERATED_CHUNKS)
        get_filename_component(cname "${chunk}" NAME_WE)
        set(want ${SPLAT_SPLIT_ALL})
        if(NOT want AND cname MATCHES "_rpx1_([0-9A-Fa-f]+)$")
            list(FIND SPLAT_FAST_LIST "${CMAKE_MATCH_1}" at)
            if(at GREATER -1)
                set(want ON)
            endif()
        endif()
        if(want)
            list(APPEND SPLAT_FAST_SRC "${chunk}")
        endif()
    endforeach()
    # the splitter (built here once, and again when its source changes)
    set(SPLAT_SPLIT_EXE "${CMAKE_BINARY_DIR}/chunk_split${CMAKE_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${SPLAT_SPLIT_EXE}" OR "${SPLAT_SPLIT_SRC}" IS_NEWER_THAN "${SPLAT_SPLIT_EXE}")
        message(STATUS "Building the game-code splitter...")
        try_compile(SPLAT_SPLIT_BUILT "${CMAKE_BINARY_DIR}/chunk_split_build"
            SOURCES "${SPLAT_SPLIT_SRC}"
            COPY_FILE "${SPLAT_SPLIT_EXE}"
            OUTPUT_VARIABLE SPLAT_SPLIT_LOG)
        if(NOT SPLAT_SPLIT_BUILT)
            message(WARNING "The game-code splitter didn't build; game code stays as it was.\n${SPLAT_SPLIT_LOG}")
            file(REMOVE "${SPLAT_SPLIT_EXE}")
        endif()
    endif()
    if(SPLAT_FAST_SRC AND EXISTS "${SPLAT_SPLIT_EXE}")
        set(split_dir "${CMAKE_BINARY_DIR}/split_chunks")
        file(MAKE_DIRECTORY "${split_dir}")
        set(split_list "")
        set(split_out "")
        foreach(chunk IN LISTS SPLAT_FAST_SRC)
            get_filename_component(cfile "${chunk}" NAME)
            string(APPEND split_list "${chunk}|${split_dir}/${cfile}\n")
            list(APPEND split_out "${split_dir}/${cfile}")
        endforeach()
        file(WRITE "${CMAKE_BINARY_DIR}/split_list.txt" "${split_list}")
        # Where the game was seen coming into the split code off the entry
        # points the splitter works out (the runtime notes those in
        # debug_output/split_entries.txt): made entry points too. The file
        # changing makes the next build split the game code again.
        set(SPLAT_ENTRIES "${CMAKE_CURRENT_SOURCE_DIR}/debug_output/split_entries.txt")
        if(NOT EXISTS "${SPLAT_ENTRIES}")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/debug_output")
            file(WRITE "${SPLAT_ENTRIES}" "# where the game came into the optimized game code off its entry points (written by the runtime)\n")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPLAT_ENTRIES}")
        # Entry points no single file shows (jumps/calls from other game-code
        # files, code addresses in the game's data: vtables, switch tables).
        # v0.19.0 split without these and fell back to the slow copy millions
        # of times (02D33FC0: a loop branching back across a file boundary).
        # Without this file every instruction stays an entry point (--all-labels).
        set(SPLAT_ENTRIES_STATIC "${CMAKE_CURRENT_SOURCE_DIR}/debug_output/split_entries_static.txt")
        if(EXISTS "${SPLAT_ENTRIES_STATIC}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPLAT_ENTRIES_STATIC}")
            set(SPLAT_SPLIT_MODE --entries "${SPLAT_ENTRIES_STATIC}")
        else()
            set(SPLAT_SPLIT_MODE --all-labels)
        endif()
        # --calls: calls in the game code come straight back to the piece
        # they were made from (splat_call in runtime/main.c)
        # --regcache3: the game's registers (integer, condition -- field 0 on its own --, XER, link,
        # count, next address, cycle counter) kept in local variables inside
        # each piece (written back when it leaves or calls out)
        # --membase: game memory through a base pointer each piece reads once
        # SPLAT_PROBE_RANGES (debugging): "lo-hi;lo-hi" hex ranges that get an
        # execution counter before every instruction (SPLAT_PROBE_DUMP=1 at run time)
        set(SPLAT_PROBE_RANGES "" CACHE STRING "Game-code ranges with execution counters (debugging), e.g. 02970000-029A0000;02CE0000-02D20000")
        set(probe_args "")
        foreach(r IN LISTS SPLAT_PROBE_RANGES)
            list(APPEND probe_args --probe ${r})
        endforeach()
        # --direct / --abi: calls to fixed addresses run the callee's file
        # function directly, and afterwards only r0/r3-r12 are read back (the
        # PowerPC calling rules; the compiler's register save/restore helpers
        # at 02D55000-02D55AAC are the exception). 02CEDA84 keeps going
        # through splat_call: the runtime waits there for the sound system
        # (wait_for_sound_system in runtime/main.c).
        option(SPLAT_DIRECT_CALLS "Direct calls between game-code files (chunk_split --direct --abi)" ON)
        set(direct_args "")
        if(SPLAT_DIRECT_CALLS)
            set(direct_args --direct --abi --keep-call 02CEDA84 --abi-except 02D55000-02D55AAC)
            # --funcs: a piece per game function, calls inside a file call the
            # callee's piece (the compiler may inline it): plaza main-thread work
            # 23.8 -> 20.7 ms standing, 22.5 -> 19.7 ms shooting (PGO both ways)
            option(SPLAT_FUNCS "Game code split into a piece per function (chunk_split --funcs)" ON)
            if(SPLAT_FUNCS)
                list(APPEND direct_args --funcs)
            endif()
        endif()
        execute_process(COMMAND "${SPLAT_SPLIT_EXE}" --size 192 ${SPLAT_SPLIT_MODE} --calls --regcache3 --membase ${direct_args} ${probe_args} --entries "${SPLAT_ENTRIES}" --list "${CMAKE_BINARY_DIR}/split_list.txt"
            RESULT_VARIABLE split_result OUTPUT_VARIABLE split_msg ERROR_VARIABLE split_err)
        if(split_result EQUAL 0)
            string(STRIP "${split_msg}" split_msg)
            list(LENGTH split_out split_count)
            message(STATUS "Game-code files built fully optimized: ${split_count} (${split_msg})")
            set(SPLAT_FAST_CHUNKS ${split_out})
            list(REMOVE_ITEM GENERATED_CHUNKS ${SPLAT_FAST_SRC})
        else()
            message(WARNING "Splitting the game code didn't work; it stays as it was.\n${split_msg}\n${split_err}")
        endif()
    endif()
endif()
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.c")
# The glslang wrapper is its own little library so that glslang's include
# folders and settings never reach the game-code files (changing their
# compile settings would make Visual Studio rebuild all of them).
list(REMOVE_ITEM RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/runtime/glsl_compile.c")
add_library(splat_glsl STATIC runtime/glsl_compile.c)
target_include_directories(splat_glsl PRIVATE runtime "${DOLRECOMP_DIR}/src" third_party/glslang)
target_link_libraries(splat_glsl PRIVATE glslang glslang-default-resource-limits)

# (DolRecomp's cpu/cpu.c is built through runtime/cpu_fast.c, which includes it.)
add_executable(${RUNNER_NAME}
    ${RUNTIME_SOURCES}
    ${GENERATED_CHUNKS}
    ${SPLAT_ATOMICS_SRC}
)
target_include_directories(${RUNNER_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    "${DOLRECOMP_DIR}/src"
    "${GENERATED_DIR}"
)
target_link_libraries(${RUNNER_NAME} PRIVATE ZLIB::ZLIB Threads::Threads glfw volk splat_glsl)
if(WIN32)
    # the built-in NSR launcher window (runtime/nsr_wizard.c): WebView2, the file pickers, SHA-256
    # (the window is WebView2: Microsoft's static loader from the Microsoft.Web.WebView2 NuGet package, launcher/third_party/webview2)
    target_link_libraries(${RUNNER_NAME} PRIVATE ole32 shell32 user32 gdi32 advapi32 version shlwapi
        "${CMAKE_CURRENT_SOURCE_DIR}/../launcher/third_party/webview2/WebView2LoaderStatic.lib")
endif()
# Point the generated code at runtime/fast_cpu.h (direct guest memory access)
# instead of DolRecomp's cpu/cpu.h.
# Loops in game code check in with the run loop every N cycles; 2048 instead
# of DolRecomp's 256 means fewer round trips.
target_compile_definitions(${RUNNER_NAME} PRIVATE DOLRECOMP_CPU_HEADER="fast_cpu.h"
    DOLRECOMP_C_LOOP_CYCLE_BUDGET=2048)

if(MSVC)
    # Generated files are huge single functions.
    # /MP: compile many files at once, one per CPU core. Without it Visual
    # Studio builds the thousands of game-code files one at a time.
    target_compile_options(${RUNNER_NAME} PRIVATE /MP /bigobj /W2
        /wd4244 /wd4267 /wd4146 /wd4018 /wd4101 /wd4102 /wd4715)
    target_compile_definitions(${RUNNER_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Recompiled functions are huge and call each other deeply: give the
    # program 256 MB of stack instead of Windows' default 1 MB.
    target_link_options(${RUNNER_NAME} PRIVATE /STACK:268435456)
    # A link map (splatoon_runner.map) names the addresses in the profiler's
    # debug_output/host_profile.txt.
    target_link_options(${RUNNER_NAME} PRIVATE /MAP)
    # SPLAT_RT_DEBUG=ON: line numbers for the drawing thread's files (a .pdb
    # next to the program), to see which lines the profile's addresses are
    option(SPLAT_RT_DEBUG "Debug line info for the renderer files (profiling)" OFF)
    if(SPLAT_RT_DEBUG)
        set_source_files_properties(runtime/latte_render.c runtime/latte_cp.c runtime/gx2.c runtime/gx2_cmd.c PROPERTIES COMPILE_OPTIONS "/Zi")
        target_link_options(${RUNNER_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()
    # A window program (no console window when double-clicked); messages go
    # to the log file (redirected output still works for test runs).
    target_link_options(${RUNNER_NAME} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    # "Play Splatoon.exe" in the main folder is the game itself: a hard link
    # to this program (no 600 MB copy), remade after each build. Double-
    # clicked (no arguments) it finds the folders and writes the log itself
    # (setup_double_clicked in runtime/main.c).
    # (SPLAT_LINK_PLAY=OFF: experimental builds leave Play Splatoon.exe alone)
    option(SPLAT_LINK_PLAY "Point the main folder's Play Splatoon.exe at this build" ON)
    if(SPLAT_LINK_PLAY)
        add_custom_command(TARGET ${RUNNER_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} "-DSRC=$<TARGET_FILE:${RUNNER_NAME}>" "-DDST=${CMAKE_CURRENT_SOURCE_DIR}/../Play Splatoon.exe"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/link_game.cmake"
            VERBATIM)
    endif()
    # The split game-code files are built with Clang (clang-cl, Visual
    # Studio's "C++ Clang tools" component) when it's there: on this code --
    # a whole game's machine code as C -- it makes far faster code than
    # Visual Studio's own compiler (keeps the registers in registers, drops
    # condition-register bits nobody reads, ...). Ninja runs it on all the
    # files in parallel. SPLAT_NO_CLANG=ON: Visual Studio's compiler.
    option(SPLAT_NO_CLANG "Build the optimized game code with Visual Studio's compiler, not clang-cl" OFF)
    # Visual Studio's folder: the generator says (Visual Studio generator), or
    # it's found from the compiler's path (Ninja generator, from a VS
    # developer prompt: .../VC/Tools/MSVC/<version>/bin/.../cl.exe).
    set(SPLAT_VS_DIR "${CMAKE_GENERATOR_INSTANCE}")
    if(NOT SPLAT_VS_DIR AND CMAKE_C_COMPILER MATCHES "^(.*)/VC/Tools/MSVC/")
        set(SPLAT_VS_DIR "${CMAKE_MATCH_1}")
    endif()
    set(SPLAT_CLANG_CL "${SPLAT_VS_DIR}/VC/Tools/Llvm/x64/bin/clang-cl.exe")
    set(SPLAT_NINJA "${SPLAT_VS_DIR}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe")
    # how many game-code files Clang builds at once: every logical core (each
    # takes up to 1-2 GB of memory)
    cmake_host_system_information(RESULT splat_cores QUERY NUMBER_OF_LOGICAL_CORES)
    set(SPLAT_CLANG_JOBS "${splat_cores}" CACHE STRING "Game-code files compiled at once by clang-cl")
    if(SPLAT_FAST_CHUNKS AND NOT SPLAT_NO_CLANG AND EXISTS "${SPLAT_CLANG_CL}" AND EXISTS "${SPLAT_NINJA}")
        set(clang_flags "/nologo /c /O2 /DNDEBUG /MD /W0 /bigobj -fno-strict-aliasing -ffp-contract=off")
        # The processors it runs on: x86-64-v3 = Intel 4th gen (2013) / AMD
        # Ryzen and newer (AVX2, BMI2, MOVBE: the game's big-endian memory
        # reads and writes become single instructions). "" = any x86-64.
        set(SPLAT_CLANG_ARCH "x86-64-v3" CACHE STRING "-march for the Clang-built game code (empty: generic x86-64)")
        if(SPLAT_CLANG_ARCH)
            string(APPEND clang_flags " /clang:-march=${SPLAT_CLANG_ARCH}")
        endif()
        # Profile-guided optimization of the Clang-built game code:
        #   SPLAT_PGO=gen -> counters in every branch; the game writes
        #                    pgo/splat-<pid>.profraw when it exits
        #   (then: llvm-profdata merge -o pgo/splat.profdata pgo/*.profraw)
        #   SPLAT_PGO=use -> built with pgo/splat.profdata: hot paths laid out
        #                    straight, register spills moved to the cold paths
        # SPLAT_LTO=ON: the Clang game code as ThinLTO bitcode, so the linker
        # (must be lld-link: configure a fresh folder with -DCMAKE_LINKER=...lld-link.exe)
        # can inline functions across the game-code files -- experiment
        option(SPLAT_LTO "ThinLTO for the Clang-built game code (needs lld-link)" OFF)
        if(SPLAT_LTO)
            string(APPEND clang_flags " -flto=thin")
        endif()
        set(SPLAT_PGO "" CACHE STRING "Profile-guided game code: gen, use or empty")
        set(SPLAT_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pgo")
        if(SPLAT_PGO STREQUAL "gen")
            file(MAKE_DIRECTORY "${SPLAT_PGO_DIR}")
            string(APPEND clang_flags " \"/clang:-fprofile-instr-generate=${SPLAT_PGO_DIR}/splat-%p.profraw\"")
        elseif(SPLAT_PGO STREQUAL "use" AND EXISTS "${SPLAT_PGO_DIR}/splat.profdata")
            string(APPEND clang_flags " \"/clang:-fprofile-instr-use=${SPLAT_PGO_DIR}/splat.profdata\" /clang:-Wno-profile-instr-out-of-date /clang:-Wno-profile-instr-unprofiled")
        endif()
        string(APPEND clang_flags " \"-DDOLRECOMP_CPU_HEADER=\\\"fast_cpu.h\\\"\" -DDOLRECOMP_C_LOOP_CYCLE_BUDGET=2048 -D_CRT_SECURE_NO_WARNINGS")
        foreach(inc "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${DOLRECOMP_DIR}/src" "${GENERATED_DIR}")
            string(APPEND clang_flags " \"/I${inc}\"")
        endforeach()
        # (builddir: this inner ninja keeps its build log apart -- with the
        # Ninja generator the outer ninja's log is in the same folder, and two
        # ninjas writing one log corrupted it: every build rebuilt everything)
        set(nj "ninja_required_version = 1.3\nbuilddir = clang_state\ncflags = ${clang_flags}\n")
        string(APPEND nj "rule cc\n  command = \"${SPLAT_CLANG_CL}\" $cflags /showIncludes /Fo$out $in\n  deps = msvc\n  description = clang-cl $in\n")
        set(clang_objs "")
        foreach(src IN LISTS SPLAT_FAST_CHUNKS)
            get_filename_component(n "${src}" NAME_WE)
            string(APPEND nj "build clang_obj/${n}.obj: cc split_chunks/${n}.c\n")
            list(APPEND clang_objs "${CMAKE_BINARY_DIR}/clang_obj/${n}.obj")
        endforeach()
        file(WRITE "${CMAKE_BINARY_DIR}/clang_chunks.ninja.new" "${nj}")
        configure_file("${CMAKE_BINARY_DIR}/clang_chunks.ninja.new" "${CMAKE_BINARY_DIR}/clang_chunks.ninja" COPYONLY)
        # (SPLAT_CLANG_JOBS at a time: every logical core by default)
        add_custom_target(splat_clang_chunks ALL
            COMMAND "${SPLAT_NINJA}" -C "${CMAKE_BINARY_DIR}" -f clang_chunks.ninja -j ${SPLAT_CLANG_JOBS}
            BYPRODUCTS ${clang_objs}
            COMMENT "Building the optimized game code with clang-cl"
            VERBATIM)
        add_dependencies(${RUNNER_NAME} splat_clang_chunks)
        target_link_libraries(${RUNNER_NAME} PRIVATE ${clang_objs})
        if(SPLAT_LTO)   # (lld-link next to clang-cl; the link does the code generation, on every core)
            set(CMAKE_LINKER_LLD "${SPLAT_VS_DIR}/VC/Tools/Llvm/x64/bin/lld-link.exe")
            set(CMAKE_C_USING_LINKER_LLD "${CMAKE_LINKER_LLD}")
            set(CMAKE_CXX_USING_LINKER_LLD "${CMAKE_LINKER_LLD}")
            set_target_properties(${RUNNER_NAME} PROPERTIES LINKER_TYPE LLD)
            target_link_options(${RUNNER_NAME} PRIVATE /opt:lldltojobs=all "/lldltocache:${CMAKE_BINARY_DIR}/lto_cache")
        endif()
        if(SPLAT_PGO STREQUAL "gen")   # (the counters' runtime: writes the .profraw file)
            file(GLOB SPLAT_PROFILE_RT "${SPLAT_VS_DIR}/VC/Tools/Llvm/x64/lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
            target_link_libraries(${RUNNER_NAME} PRIVATE ${SPLAT_PROFILE_RT})
        endif()
        message(STATUS "Optimized game code: clang-cl (${SPLAT_CLANG_CL})${SPLAT_PGO}")
    elseif(SPLAT_FAST_CHUNKS)
        # The split game-code files: same settings as the other game-code
        # files, but optimized (the small functions build quickly; the
        # optimizer limit is lifted too, in case one is still big).
        add_library(splat_fast_chunks OBJECT ${SPLAT_FAST_CHUNKS})
        target_include_directories(splat_fast_chunks PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${DOLRECOMP_DIR}/src" "${GENERATED_DIR}")
        target_compile_definitions(splat_fast_chunks PRIVATE DOLRECOMP_CPU_HEADER="fast_cpu.h"
            DOLRECOMP_C_LOOP_CYCLE_BUDGET=2048 _CRT_SECURE_NO_WARNINGS)
        target_compile_options(splat_fast_chunks PRIVATE /MP8 /bigobj /W2 /d2OptimizeHugeFunctions
            /wd4244 /wd4267 /wd4146 /wd4018 /wd4101 /wd4102 /wd4715)
        target_sources(${RUNNER_NAME} PRIVATE $<TARGET_OBJECTS:splat_fast_chunks>)
    endif()
else()
    target_compile_options(${RUNNER_NAME} PRIVATE -w)
    target_link_libraries(${RUNNER_NAME} PRIVATE m)
    if(SPLAT_FAST_CHUNKS)
        target_sources(${RUNNER_NAME} PRIVATE ${SPLAT_FAST_CHUNKS})
    endif()
endif()

# frame_replay: draws a frame saved by the runtime's frame capture again,
# without the game (for finding rendering problems). Not built by default:
#   cmake --build build_splatoon --config Release --target frame_replay
add_executable(frame_replay EXCLUDE_FROM_ALL tools/frame_replay.c runtime/latte_render.c runtime/frame_tracker.c runtime/latte_shader.c
    runtime/latte_texture.c runtime/latte_capture.c runtime/vk_ctx.c runtime/gx2_layout.c)
target_include_directories(frame_replay PRIVATE runtime "${DOLRECOMP_DIR}/src")
target_link_libraries(frame_replay PRIVATE ZLIB::ZLIB Threads::Threads glfw volk splat_glsl)
if(MSVC)
    target_compile_definitions(frame_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_link_libraries(frame_replay PRIVATE m)
endif()

# rpx_patch: fills in an .rpx's import addresses before recompiling.
add_executable(rpx_patch tools/rpx_patch.c)
target_include_directories(rpx_patch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
target_link_libraries(rpx_patch PRIVATE ZLIB::ZLIB)
if(MSVC)
    target_compile_definitions(rpx_patch PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The old launcher (tools/launcher.c: built a newer zip, then started the
# game as a second program). "Play Splatoon.exe" is now the game itself (see
# the hard link above); this is only built on request.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/launcher.c")   # (not in the release zips)
    add_executable(play_splatoon WIN32 EXCLUDE_FROM_ALL tools/launcher.c)
    set_target_properties(play_splatoon PROPERTIES OUTPUT_NAME "Play Splatoon Launcher")
    target_compile_definitions(play_splatoon PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()
