# Require a relatively modern version of CMake
cmake_minimum_required(VERSION 3.15)

# Name your project and declare it as a C++ project
project(LobProject CXX)

# Force the compiler to use C++20 (Modern C++)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Find the libraries installed by vcpkg
find_package(Boost REQUIRED)
find_package(simdjson CONFIG REQUIRED)

# 2. Tell CMake to build an executable named 'lob_app' from your main.cpp
add_executable(lob_app src/main.cpp)

# 3. Link the Boost and simdjson libraries to your executable
target_link_libraries(lob_app PRIVATE
    Boost::headers
    simdjson::simdjson
)