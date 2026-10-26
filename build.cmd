@echo off
setlocal
cd /d "%~dp0"
if defined VSCMD_ARG_TGT_ARCH if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Microsoft Visual C Build Tools are required. & exit /b 1)
for /f "usebackq tokens=*" %%v in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "MG_VS=%%v"
if not defined MG_VS (echo No x64 MSVC installation found. & exit /b 1)
call "%MG_VS%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
:ready
if not exist build mkdir build
if not exist bin mkdir bin
set "MG_CFLAGS=/nologo /TC /std:c17 /utf-8 /W4 /WX /O1 /GL /MT /GS /sdl /guard:cf /D_CRT_SECURE_NO_WARNINGS"
set "MG_LIBS=ole32.lib uuid.lib user32.lib gdi32.lib shell32.lib comctl32.lib advapi32.lib"
rc /nologo /I src /fo build\MuteGuard.res src\MuteGuard.rc
if errorlevel 1 exit /b 1
cl %MG_CFLAGS% /c /Fobuild\ src\main.c src\config.c src\audio.c src\ui.c
if errorlevel 1 exit /b 1
link /nologo build\main.obj build\config.obj build\audio.obj build\ui.obj build\MuteGuard.res %MG_LIBS% /OUT:bin\MuteGuard.exe /LTCG /OPT:REF,ICF /INCREMENTAL:NO /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF /MACHINE:X64 /SUBSYSTEM:WINDOWS,10.0 /MANIFEST:EMBED /MANIFESTINPUT:src\MuteGuard.manifest
if errorlevel 1 exit /b 1
if /i "%~1"=="test" goto tests
if /i not "%~1"=="test-build" exit /b 0
:tests
cl %MG_CFLAGS% /I src /Fobuild\ tests\test_config.c /Febuild\test_config.exe /link build\config.obj %MG_LIBS% /LTCG /OPT:REF,ICF /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64
if errorlevel 1 exit /b 1
if /i "%~1"=="test" build\test_config.exe
if errorlevel 1 exit /b 1
cl %MG_CFLAGS% /I src /Fobuild\ tests\test_audio.c /Febuild\test_audio.exe /link build\config.obj build\audio.obj %MG_LIBS% /LTCG /OPT:REF,ICF /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64
if errorlevel 1 exit /b 1
cl %MG_CFLAGS% /I src /Fobuild\ tests\test_ui.c /Febuild\test_ui.exe /link build\config.obj build\audio.obj %MG_LIBS% /LTCG /OPT:REF,ICF /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64 /MANIFEST:EMBED /MANIFESTINPUT:src\MuteGuard.manifest
if errorlevel 1 exit /b 1
cl %MG_CFLAGS% /I src /Fobuild\ tests\test_tray.c /Febuild\test_tray.exe /link build\ui.obj build\config.obj build\audio.obj build\MuteGuard.res %MG_LIBS% /LTCG /OPT:REF,ICF /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64 /MANIFEST:EMBED /MANIFESTINPUT:src\MuteGuard.manifest
if errorlevel 1 exit /b 1
cl %MG_CFLAGS% /DMG_TESTING /c /Fobuild\audio_test.obj src\audio.c
if errorlevel 1 exit /b 1
cl %MG_CFLAGS% /DMG_TESTING /I src /Fobuild\ tests\test_automatic.c /Febuild\test_automatic.exe /link build\config.obj build\audio_test.obj %MG_LIBS% /LTCG /OPT:REF,ICF /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64
if errorlevel 1 exit /b 1
cl %MG_CFLAGS% /I src /Fobuild\ tests\test_control.c /Febuild\test_control.exe /link %MG_LIBS% /LTCG /OPT:REF,ICF /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64
if errorlevel 1 exit /b 1
if /i not "%~1"=="test" exit /b 0
build\test_audio.exe
if errorlevel 1 exit /b 1
build\test_ui.exe
if errorlevel 1 exit /b 1
build\test_tray.exe
if errorlevel 1 exit /b 1
build\test_automatic.exe
exit /b %errorlevel%
