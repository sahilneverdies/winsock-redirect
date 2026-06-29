@echo off
REM ==============================================================
REM Build script for libhooks.so + hooker (ARM64)
REM Prerequisites: Android NDK installed, NDK_PATH set or passed
REM ==============================================================

if "%1"=="" (
    if not defined NDK_PATH (
        echo Usage: build.bat C:\path\to\android-ndk-r26b
        echo Or set NDK_PATH environment variable first.
        exit /b 1
    )
) else (
    set NDK_PATH=%1
)

set TOOLCHAIN=%NDK_PATH%\toolchains\llvm\prebuilt\windows-x86_64
set CC=%TOOLCHAIN%\bin\aarch64-linux-android21-clang
set STRIP=%TOOLCHAIN%\bin\aarch64-linux-android-strip

echo Compiling libhooks.so...
%CC% -shared -fPIC -O2 -Wall -Wextra libhooks.c -o libhooks.so -ldl -llog
if %ERRORLEVEL% neq 0 exit /b 1
%STRIP% libhooks.so
echo OK - libhooks.so

echo Compiling hooker (static)...
%CC% -static -O2 -Wall hooker.c -o hooker -ldl
if %ERRORLEVEL% neq 0 exit /b 1
%STRIP% hooker
echo OK - hooker

echo.
echo Both binaries ready. Push to emulator:
echo   adb push libhooks.so /data/local/tmp/
echo   adb push hooker /data/local/tmp/
echo   adb shell chmod 755 /data/local/tmp/hooker
echo.
echo Inject into running game:
echo   adb shell su -c "/data/local/tmp/hooker com.dts.freefireth /data/local/tmp/libhooks.so"
echo.
