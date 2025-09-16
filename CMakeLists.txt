cmake_minimum_required(VERSION 4.0.0 FATAL_ERROR)

project(DishonoredObjects VERSION 0.1.0 LANGUAGES CXX C)
file(GLOB imgui_sources "imgui/*.cpp" "imgui/backends/imgui_impl_dx11.cpp" "imgui/backends/imgui_impl_win32.cpp")
add_executable(DishonoredObjects main.cpp OverlayPoints.hpp ProcHelper.hpp MemoryScanner.hpp OverlayD3D.hpp OverlayImGuiDX11.hpp ${imgui_sources})

set_property(TARGET DishonoredObjects PROPERTY CXX_STANDARD 20)

target_include_directories(DishonoredObjects PRIVATE imgui)