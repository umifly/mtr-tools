@echo off
setlocal
echo ========================================================
echo  MTR-Tools MinGW-w64 Build Script
echo ========================================================

set "PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;%PATH%"

cd /d "%~dp0"

echo [1/3] Compiling test_analyzer.exe ...
g++.exe -std=c++20 -O2 -Isrc tests/test_analyzer.cpp src/core/geo_asn.cpp src/core/analyzer.cpp -o "%~dp0test_analyzer.exe" -lws2_32 -lwinhttp -static
if errorlevel 1 (
    echo [ERROR] test_analyzer build failed!
    exit /b 1
)
echo [1/3] test_analyzer.exe build SUCCESS.

echo [2/3] Compiling mtr-tools.exe (TUI + GUI) ...
g++.exe -std=c++20 -O2 -Isrc src/main.cpp src/core/prober.cpp src/core/prober_win.cpp src/core/geo_asn.cpp src/core/analyzer.cpp src/tui/tui_app.cpp src/gui/gui_win32.cpp -o "%~dp0mtr-tools.exe" -mwindows -liphlpapi -lws2_32 -lwinhttp -lcomctl32 -lgdi32 -luser32 -static -static-libgcc -static-libstdc++
if errorlevel 1 (
    echo [ERROR] mtr-tools build failed!
    exit /b 1
)
echo [2/3] mtr-tools.exe build SUCCESS.

echo [3/3] Running automated unit tests ...
"%~dp0test_analyzer.exe"
if errorlevel 1 (
    echo [ERROR] Unit tests failed!
    exit /b 1
)

echo.
echo ========================================================
echo  Build and Tests PASSED!
echo  Run TUI mode:  mtr-tools.exe 223.5.5.5
echo  Run GUI mode:  mtr-tools.exe --gui 223.5.5.5
echo  Export report: mtr-tools.exe --cli 223.5.5.5 -c 5 -o report.md
echo ========================================================
