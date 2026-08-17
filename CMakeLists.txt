cmake_minimum_required(VERSION 3.20)
project(daw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DAW_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(DAW_TSAN "Build with ThreadSanitizer" OFF)

add_library(daw_engine STATIC
    engine/src/Engine.cpp
    engine/src/Oscillator.cpp
)
target_include_directories(daw_engine PUBLIC engine/include)
target_compile_options(daw_engine PUBLIC -Wall -Wextra -Wpedantic -Wshadow -Werror)

if(DAW_ASAN)
    target_compile_options(daw_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(daw_engine PUBLIC -fsanitize=address,undefined)
endif()

if(DAW_TSAN)
    target_compile_options(daw_engine PUBLIC -fsanitize=thread -g)
    target_link_options(daw_engine PUBLIC -fsanitize=thread)
endif()

find_package(Threads REQUIRED)
target_link_libraries(daw_engine PUBLIC Threads::Threads)

add_subdirectory(cli)

enable_testing()
add_subdirectory(tests)
