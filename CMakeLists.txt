cmake_minimum_required(VERSION 3.28)
project(fathom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FATHOM_BUILD_TUTORIALS "Build the Databento tutorial programs (needs OpenSSL and zstd)" OFF)
if(FATHOM_BUILD_TUTORIALS)
    include(FetchContent)
    FetchContent_Declare(
            databento
            GIT_REPOSITORY https://github.com/databento/databento-cpp
            GIT_TAG v0.68.1
    )
    FetchContent_MakeAvailable(databento)
endif()

# Header-only engine code and the warning flags, shared by the executable and the tests.
add_library(fathom_core INTERFACE)
target_include_directories(fathom_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/engine/src)
target_compile_options(fathom_core INTERFACE -Wall -Wextra -Wpedantic)

add_executable(fathom engine/src/main.cpp)
target_link_libraries(fathom PRIVATE fathom_core)
# -O3 and the host's instruction set for the engine binary only; the
# tests keep the default flags so the sanitizers see the plain code.
target_compile_options(fathom PRIVATE -O3 -march=native)

# Profile-guided optimization, in two configures (see scripts/pgo.sh).
# Off by default so a fresh clone builds without a profile.
set(FATHOM_PGO "" CACHE STRING "PGO phase for the engine binary: generate, use, or empty")
if(FATHOM_PGO STREQUAL "generate")
    target_compile_options(fathom PRIVATE -fprofile-generate=${CMAKE_BINARY_DIR}/pgo)
    target_link_options(fathom PRIVATE -fprofile-generate=${CMAKE_BINARY_DIR}/pgo)
elseif(FATHOM_PGO STREQUAL "use")
    target_compile_options(fathom PRIVATE -fprofile-use=${CMAKE_BINARY_DIR}/pgo -fprofile-correction)
endif()

# The tests run under ASan and UBSan; no-recover makes a UBSan report fail the run.
enable_testing()
add_executable(itch_test engine/tests/itch_test.cpp)
target_link_libraries(itch_test PRIVATE fathom_core)
target_compile_options(itch_test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all
                       -fno-omit-frame-pointer)
target_link_options(itch_test PRIVATE -fsanitize=address,undefined)
add_test(NAME itch_test COMMAND itch_test)

if(FATHOM_BUILD_TUTORIALS)
    add_executable(order_actions tutorials/01_order_actions.cpp)
    target_link_libraries(order_actions PRIVATE databento::databento)

    add_executable(order_tracking tutorials/02_order_tracking.cpp)
    target_link_libraries(order_tracking PRIVATE databento::databento)

    add_executable(limit_order_book tutorials/03_limit_order_book.cpp)
    target_link_libraries(limit_order_book PRIVATE databento::databento)
endif()
