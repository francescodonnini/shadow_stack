cmake_minimum_required(VERSION 3.31.6)
project(shadow-stack)

set(CMAKE_CXX_STANDARD 17)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")

option(OPTS_ALL "Enable all optimizations" OFF)
if(OPTS_ALL)
    add_definitions(-DOPT_ALL)
endif()

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIR})

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)

if (NOT ${LLVM_ENABLE_RTTI})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
    message(STATUS "disable RTTI")
endif()

add_llvm_pass_plugin(ShadowStack
    ShadowStack.cpp)