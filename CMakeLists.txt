cmake_minimum_required(VERSION 4.0.0 FATAL_ERROR)

project(DishonoredObjects VERSION 0.1.0 LANGUAGES CXX C)

add_executable(DishonoredObjects main.cpp OverlayPoints.hpp ProcHelper.hpp MemoryScanner.hpp)

set_property(TARGET DishonoredObjects PROPERTY CXX_STANDARD 20)