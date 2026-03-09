# Require a relatively modern version of CMake
cmake_minimum_required(VERSION 3.15)

# Name your project and declare it as a C++ project
project(LobProject CXX)

# Force the compiler to use C++20 (Modern C++)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ─── Tests ────────────────────────────────────────────────────────────────────

option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_TESTS_ONLY "Skip the main app and its heavy dependencies (for CI test jobs)" OFF)
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(NOT BUILD_TESTS_ONLY)
    find_package(ixwebsocket CONFIG REQUIRED)
    find_package(httplib CONFIG REQUIRED)

    # Tell CMake to build an executable named 'lob_app' from your main.cpp
    add_executable(lob_app
        src/main.cpp
        src/order_book.cpp
        src/binance_adapter.cpp
        src/kraken_adapter.cpp
    )

    # Link the ixwebsocket and nlohmann_json libraries to your executable
    target_link_libraries(lob_app PRIVATE
        Threads::Threads
        ixwebsocket::ixwebsocket
        nlohmann_json::nlohmann_json
        httplib::httplib
    )
endif()

if(BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)

    add_executable(lob_tests
        tests/test_order_book.cpp
        tests/test_decimal.cpp
        tests/test_safe_arithmetic.cpp
        tests/test_binance_utils.cpp
        tests/test_prometheus_format.cpp
        tests/test_subscriber_server.cpp
        tests/test_symbol_normalizer.cpp
        tests/test_kraken_utils.cpp
        src/order_book.cpp
    )

    target_include_directories(lob_tests PRIVATE src)

    target_link_libraries(lob_tests PRIVATE
        Threads::Threads
        nlohmann_json::nlohmann_json
        GTest::gtest_main
    )

    if(ENABLE_COVERAGE)
        target_compile_options(lob_tests PRIVATE --coverage -O0)
        target_link_options(lob_tests PRIVATE --coverage)
    endif()

    include(GoogleTest)
    gtest_discover_tests(lob_tests)
endif()