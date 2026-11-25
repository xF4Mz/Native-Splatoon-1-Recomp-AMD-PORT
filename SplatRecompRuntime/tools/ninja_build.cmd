@echo off
rem Visual Studio's compiler environment, then CMake with the Ninja generator.
rem usage: ninja_build.cmd configure | build
rem Set these first if yours differ (see the README's "How to build"):
rem   VS             your Visual Studio 2022 folder
rem   GENERATED_DIR  the folder DolRecomp wrote (the one with generated.h)
rem   ZLIB_DIR       a static zlib (include\ and lib\zlib.lib)
rem   BD             the build folder (default: build_ninja next to this project)
rem   PGO            "use" with a pgo\splat.profdata profile, else leave it empty
set "ROOT=%~dp0.."
if "%VS%"=="" set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
if "%BD%"=="" set "BD=%ROOT%\build_ninja"
if "%GENERATED_DIR%"=="" set "GENERATED_DIR=%ROOT%\..\splat_recomp\generated"
if "%ZLIB_DIR%"=="" set "ZLIB_DIR=%ROOT%\..\zlib"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%BD%\tmp" mkdir "%BD%\tmp"
set "TMP=%BD%\tmp"
set "TEMP=%BD%\tmp"
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if "%1"=="configure" "%CMAKE%" -S "%ROOT%" -B "%BD%" -G Ninja -DCMAKE_MAKE_PROGRAM="%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DSPLAT_PGO=%PGO% -DSPLAT_CLANG_ARCH=x86-64-v3 -DSPLAT_DIRECT_CALLS=ON "-DGENERATED_DIR=%GENERATED_DIR%" "-DDOLRECOMP_DIR=%ROOT%\..\DolRecomp" "-DZLIB_INCLUDE_DIR=%ZLIB_DIR%\include" "-DZLIB_LIBRARY=%ZLIB_DIR%\lib\zlib.lib" %EXTRA%
if "%1"=="build" "%CMAKE%" --build "%BD%"
