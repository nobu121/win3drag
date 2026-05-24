@echo off
setlocal

where g++ >nul 2>&1
if errorlevel 1 (
    echo [error] MinGW-w64 g++ not found in PATH.
    echo Install MinGW-w64 and ensure g++.exe is on PATH, then re-run build.bat.
    exit /b 1
)

echo Compiling 3drag.exe ...

g++ -std=c++17 -Os -s -municode ^
    -fno-exceptions -fno-rtti ^
    -ffunction-sections -fdata-sections ^
    -static -static-libgcc -static-libstdc++ ^
    -DUNICODE -D_UNICODE ^
    -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 ^
    -Wall -Wextra -Wno-unused-parameter ^
    -o 3drag.exe main.cpp ^
    -Wl,--gc-sections ^
    -lhid -ladvapi32 -lshell32

if errorlevel 1 (
    echo [error] Build failed.
    exit /b 1
)

echo.
echo [ok] Built 3drag.exe
for %%I in (3drag.exe) do echo      size: %%~zI bytes

endlocal
