@echo off
rem ============================================================
rem  build.cmd - build the AW2033 userspace driver artifacts:
rem
rem    libaw2033.a        static lib for consumers (chgd in the
rem                       shark8-led-daemon project links this)
rem    tools\awctl        standalone CLI for on-device chip debug
rem
rem  To update a consumer that vendors libaw2033.a (e.g. the
rem  shark8-led-daemon project), copy the freshly built
rem  libaw2033.a + aw2033.h into its aw2033-driver\ folder.
rem
rem  Override the compiler with:   set NDK_CC=path\to\clang.cmd
rem ============================================================
setlocal
if not defined NDK_CC set "NDK_CC=D:\System\Apps\Android NDK\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang.cmd"

echo [1/2] Building libaw2033.a...
call "%NDK_CC%" -O2 -Wall -Wno-comment -I"%~dp0." -c "%~dp0aw2033.c" -o "%~dp0aw2033.o"
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
"%NDK_CC%\..\llvm-ar.exe" rcs "%~dp0libaw2033.a" "%~dp0aw2033.o"
if errorlevel 1 (
    echo AR FAILED
    del "%~dp0aw2033.o" 2>nul
    exit /b 1
)
del "%~dp0aw2033.o" 2>nul

echo [2/2] Building tools\awctl...
call "%NDK_CC%" -O2 -s -Wall -Wno-comment -I"%~dp0." -o "%~dp0tools\awctl" "%~dp0tools\awctl.c" "%~dp0aw2033.c"
if errorlevel 1 (
    echo awctl BUILD FAILED
    exit /b 1
)

echo DONE: %~dp0libaw2033.a, %~dp0tools\awctl