cmake_minimum_required(VERSION 4.0.0 FATAL_ERROR)

project(DishonoredObjects VERSION 0.1.0 LANGUAGES CXX C)

file(GLOB sources  "*.h" "*.cpp" "*.hpp" "MinHook/include/*.h")
file(GLOB imgui "imgui/*.cpp" "imgui/backends/imgui_impl_dx11.cpp" "imgui/backends/imgui_impl_win32.cpp")

add_library(DishonoredObjects SHARED ${sources} ${imgui})

set_property(TARGET DishonoredObjects PROPERTY CXX_STANDARD 20)

target_include_directories(DishonoredObjects PRIVATE imgui)

foreach(source IN ITEMS ${sources} ${imgui})
    get_filename_component(source_path "${source}" DIRECTORY)
    # Получаем относительный путь от корня проекта
    file(RELATIVE_PATH source_group_path "${CMAKE_CURRENT_SOURCE_DIR}" "${source_path}")
    if(source_group_path STREQUAL "")
        set(source_group_path "src")
    endif()
    # Заменяем слеши на обратные для VS
    string(REPLACE "/" "\\" source_group_msvc "${source_group_path}")
    source_group("${source_group_msvc}" FILES "${source}")
endforeach()