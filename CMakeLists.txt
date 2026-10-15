cmake_minimum_required(VERSION 3.28)
project(fathom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
        databento
        GIT_REPOSITORY https://github.com/databento/databento-cpp
        GIT_TAG main
)
FetchContent_MakeAvailable(databento)

add_executable(fathom engine/src/main.cpp)
target_compile_options(fathom PRIVATE -Wall -Wextra -Wpedantic)

add_executable(order_actions tutorials/01_order_actions.cpp)
target_link_libraries(order_actions PRIVATE databento::databento)

add_executable(order_tracking tutorials/02_order_tracking.cpp)
target_link_libraries(order_tracking PRIVATE databento::databento)

add_executable(limit_order_book tutorials/03_limit_order_book.cpp)
target_link_libraries(limit_order_book PRIVATE databento::databento)
