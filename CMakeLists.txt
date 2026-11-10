cmake_minimum_required(VERSION 3.16)
project(mtr-tools CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src)

set(CORE_SOURCES
    src/core/prober.cpp
    src/core/geo_asn.cpp
    src/core/analyzer.cpp
)

if(WIN32)
    list(APPEND CORE_SOURCES src/core/prober_win.cpp src/gui/gui_win32.cpp)
    set(SYSTEM_LIBS iphlpapi ws2_32 winhttp comctl32 gdi32 user32 shell32)
    set(GUI_TYPE WIN32)
else()
    list(APPEND CORE_SOURCES src/core/prober_linux.cpp)
    set(SYSTEM_LIBS pthread)
    set(GUI_TYPE "")
endif()

# 主程序
add_executable(mtr-tools ${GUI_TYPE}
    src/main.cpp
    src/tui/tui_app.cpp
    ${CORE_SOURCES}
)
target_link_libraries(mtr-tools PRIVATE ${SYSTEM_LIBS})

# 单元测试程序
add_executable(test_analyzer
    tests/test_analyzer.cpp
    src/core/geo_asn.cpp
    src/core/analyzer.cpp
)
target_link_libraries(test_analyzer PRIVATE ${SYSTEM_LIBS})
