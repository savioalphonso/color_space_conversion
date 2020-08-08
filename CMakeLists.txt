cmake_minimum_required(VERSION 3.16)
project(color_space_conversion C)

set(CMAKE_C_STANDARD 11)
find_package(TIFF REQUIRED)
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG")

add_executable(color_space_conversion main.c)
target_link_libraries(color_space_conversion TIFF::TIFF)