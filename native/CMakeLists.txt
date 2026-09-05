cmake_minimum_required(VERSION 3.18)
project(uma_landscape CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(uma_landscape SHARED native/landscape.cpp)

target_include_directories(uma_landscape PRIVATE
    ${DOBBY_DIR}/include
    ${CMAKE_SOURCE_DIR}/include
)

find_library(log-lib log)
target_link_libraries(uma_landscape ${log-lib} dl)
