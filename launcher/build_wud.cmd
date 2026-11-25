@echo off
rem Builds nsr_wud.exe (the disc image reader) with Visual Studio's compiler into %1 (default launcher\build).
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0build"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%p in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%p"
if "%VS%"=="" ( echo Visual Studio with the C++ tools is needed & exit /b 1 )
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /W3 /Fo"%OUT%\\" /Fe"%OUT%\nsr_wud.exe" "%~dp0nsr_wud.c"
if errorlevel 1 exit /b 1
echo built %OUT%\nsr_wud.exe
