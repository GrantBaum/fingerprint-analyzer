# Oldest CMake version this file works with
cmake_minimum_required(VERSION 3.16)

# Project name and language
project(fpmatch LANGUAGES CXX)

# Use C++17 and fail if the compiler can't do it
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenCV on this machine (stops with an error if it's missing)
find_package(OpenCV REQUIRED)

# Build one program called fpmatch from main.cpp
add_executable(fpmatch main.cpp)

# Let main.cpp find the OpenCV headers
target_include_directories(fpmatch PRIVATE ${OpenCV_INCLUDE_DIRS})

# Link the OpenCV libraries into the program
target_link_libraries(fpmatch PRIVATE ${OpenCV_LIBS})

# Turn on extra compiler warnings so small bugs show up early
target_compile_options(fpmatch PRIVATE -Wall -Wextra)