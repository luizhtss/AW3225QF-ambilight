cmake_minimum_required(VERSION 3.15)
project(alienware-ambilight LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to alienfx-tools source (only for DXGIManager)
set(ALIENFX_GUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../alienfx-tools/alienfx-gui")

# Disable ACPI lights (avoids AlienFan_SDK dependency)
add_definitions(-DNOACPILIGHTS)

# Main ambilight (direct HID, no SDK dependency)
add_executable(ambilight
    main.cpp
    "${ALIENFX_GUI_DIR}/DXGIManager.cpp"
)

target_include_directories(ambilight PRIVATE
    "${ALIENFX_GUI_DIR}"
)

target_link_libraries(ambilight PRIVATE
    setupapi
    hid
    d3d11
    dxgi
)

# LED Demo tool - interactive color test
add_executable(led_demo led_demo.cpp)
target_link_libraries(led_demo PRIVATE setupapi hid)

# HID debug tool (standalone)
add_executable(hid_debug hid_debug.cpp)
target_link_libraries(hid_debug PRIVATE setupapi hid)

# Suppress warnings from third-party code
if(MSVC)
    target_compile_options(ambilight PRIVATE /W3 /wd4996)
    target_compile_options(hid_debug PRIVATE /W3 /wd4996)
    target_compile_options(led_demo PRIVATE /W3 /wd4996)
endif()
