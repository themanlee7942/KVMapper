@echo off
REM ====================================================================
REM build.bat - Windows-native local build for KVMapper.
REM
REM Requires either:
REM   (a) mingw-w64 gcc on PATH (gcc, windres) - native Windows build
REM   (b) Python 3 with ziglang (pip install ziglang) - cross compile
REM
REM Usage:
REM   build.bat        - build all targets into dist\
REM   build.bat clean  - remove dist\ and object files
REM
REM No emoji, no Unicode dashes - cmd.exe code-page hostile.
REM ====================================================================

setlocal
cd /d "%~dp0"

if /i "%1"=="clean" goto :CLEAN

if not exist dist mkdir dist

REM --- Try mingw native first ---
where gcc >nul 2>nul
if %ERRORLEVEL%==0 (
    echo Using mingw-w64 gcc
    goto :BUILD_MINGW
)

REM --- Fallback: zig via Python ---
where python >nul 2>nul
if %ERRORLEVEL%==0 (
    python -c "import ziglang" >nul 2>nul
    if %ERRORLEVEL%==0 (
        echo Using zig via Python ziglang
        goto :BUILD_ZIG
    )
)

echo.
echo ERROR: Neither gcc nor python+ziglang is on PATH.
echo Install one of:
echo   - mingw-w64 (https://www.mingw-w64.org)
echo   - Python 3, then: pip install ziglang
echo.
exit /b 1

:BUILD_MINGW
set CFLAGS=-O2 -Wall -Wextra -std=c99 -Iinclude -DUNICODE -D_UNICODE
set LD_EXE=-mwindows -lcomctl32 -lshell32 -luser32 -lgdi32 -lole32
set LD_DLL=-shared -luser32 -lkernel32

echo Compiling resources...
windres -O coff app.rc -o dist\res.o
if errorlevel 1 goto :FAIL

echo Compiling kvmapper.exe (x64)...
gcc %CFLAGS% src\main.c src\capture.c src\config.c src\shared_mem.c dist\res.o -o dist\kvmapper.exe %LD_EXE%
if errorlevel 1 goto :FAIL

echo Compiling kvmapper_hook.dll (x64)...
gcc %CFLAGS% src\hook\hook.c src\hook\inject.c -o dist\kvmapper_hook.dll %LD_DLL%
if errorlevel 1 goto :FAIL

echo Compiling kvmapper_hook_x86.dll (x86)...
gcc -m32 %CFLAGS% src\hook\hook.c src\hook\inject.c -o dist\kvmapper_hook_x86.dll %LD_DLL%
if errorlevel 1 (
    echo NOTE: x86 build failed - mingw might not have multilib. Skipping.
)

copy /Y app.manifest dist\kvmapper.exe.manifest >nul
echo.
echo === Build complete ===
dir dist
exit /b 0

:BUILD_ZIG
set CFLAGS=-O2 -Wall -Wextra -std=c99 -Iinclude -DUNICODE -D_UNICODE
set LD_EXE=-Wl,--subsystem,windows -lcomctl32 -lshell32 -luser32 -lgdi32 -lole32
set LD_DLL=-shared -luser32 -lkernel32

echo Compiling kvmapper.exe (x64)...
python -m ziglang cc -target x86_64-windows-gnu %CFLAGS% src\main.c src\capture.c src\config.c src\shared_mem.c -o dist\kvmapper.exe %LD_EXE%
if errorlevel 1 goto :FAIL

echo Compiling kvmapper_hook.dll (x64)...
python -m ziglang cc -target x86_64-windows-gnu %CFLAGS% src\hook\hook.c src\hook\inject.c -o dist\kvmapper_hook.dll %LD_DLL%
if errorlevel 1 goto :FAIL

echo Compiling kvmapper_hook_x86.dll (x86)...
python -m ziglang cc -target x86-windows-gnu %CFLAGS% src\hook\hook.c src\hook\inject.c -o dist\kvmapper_hook_x86.dll %LD_DLL%
if errorlevel 1 goto :FAIL

echo Compiling kvmapper_x86.exe (x86)...
python -m ziglang cc -target x86-windows-gnu %CFLAGS% src\main.c src\capture.c src\config.c src\shared_mem.c -o dist\kvmapper_x86.exe %LD_EXE%
if errorlevel 1 (
    echo NOTE: x86 exe build failed - shipping x86 DLL only.
)

copy /Y app.manifest dist\kvmapper.exe.manifest >nul
echo.
echo === Build complete ===
dir dist
exit /b 0

:CLEAN
echo Cleaning...
if exist dist rmdir /S /Q dist
echo Done.
exit /b 0

:FAIL
echo.
echo Build FAILED.
exit /b 1
