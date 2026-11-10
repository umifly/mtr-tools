#!/usr/bin/env bash
set -e

echo "========================================================"
echo " MTR-Tools Linux Build Script (GCC / Clang C++20)"
echo "========================================================"

CXX=${CXX:-g++}

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "[ERROR] C++ compiler ($CXX) not found! Install via:"
    echo "  Ubuntu/Debian: sudo apt update && sudo apt install -y g++ make"
    echo "  RHEL/CentOS:   sudo dnf install -y gcc-c++ make"
    exit 1
fi

echo "[1/3] Compiling unit tests (test_analyzer) ..."
"$CXX" -std=c++20 -O2 -Isrc tests/test_analyzer.cpp src/core/geo_asn.cpp src/core/analyzer.cpp -o test_analyzer -pthread

echo "[2/3] Compiling main binary (mtr-tools) ..."
"$CXX" -std=c++20 -O2 -Isrc src/main.cpp src/core/prober.cpp src/core/prober_linux.cpp src/core/geo_asn.cpp src/core/analyzer.cpp src/tui/tui_app.cpp -o mtr-tools -pthread

echo "[3/3] Running automated unit tests ..."
./test_analyzer

echo ""
echo "========================================================"
echo " Build and Tests PASSED!"
echo " Run TUI mode:      ./mtr-tools 223.5.5.5"
echo " Run Web-GUI mode:  ./mtr-tools --gui 223.5.5.5"
echo " Run CLI report:    ./mtr-tools --cli 223.5.5.5 -c 5 -o report.md"
echo "========================================================"
