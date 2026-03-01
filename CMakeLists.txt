# Require a relatively modern version of CMake
cmake_minimum_required(VERSION 3.15)

# Name your project and declare it as a C++ project
project(LobProject CXX)

# Force the compiler to use C++20 (Modern C++)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Tell CMake to build an executable named 'lob_app' from your main.cpp
add_executable(lob_app
    src/main.cpp
    src/order_book.cpp
    src/feed_handler.cpp
)

# Link the ixwebsocket and nlohmann_json libraries to your executable
target_link_libraries(lob_app PRIVATE
    Threads::Threads
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
)