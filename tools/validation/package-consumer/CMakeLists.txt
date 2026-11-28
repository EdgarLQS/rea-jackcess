cmake_minimum_required(VERSION 3.16)
project(rea_package_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(rea_jackcess CONFIG REQUIRED)

add_executable(rea_package_consumer main.cpp)
target_link_libraries(rea_package_consumer PRIVATE rea_jackcess::core rea_jackcess::pgeo)

enable_testing()
add_test(NAME package_consumer COMMAND rea_package_consumer)
