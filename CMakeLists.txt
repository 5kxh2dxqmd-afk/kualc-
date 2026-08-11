cmake_minimum_required(VERSION 3.16)
project(kual_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(kual-native src/main.cpp)
target_compile_options(kual-native PRIVATE -Wall -Wextra -Wpedantic -Werror)

option(KUAL_STATIC "Produce a self-contained binary for Kindle userlands" OFF)
if(KUAL_STATIC)
  target_compile_options(kual-native PRIVATE -Os -ffunction-sections -fdata-sections)
  target_link_options(kual-native PRIVATE -static -Wl,--gc-sections)
endif()

install(TARGETS kual-native RUNTIME DESTINATION bin)
