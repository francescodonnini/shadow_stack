cmake_minimum_required(VERSION 3.31.6)
project(shadow-stack)

set(CMAKE_CXX_STANDARD 17)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")

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

option(OPT_CONSTADDR "Skip stores at constant offsets in locals/globals" OFF)
option(OPT_DOMTREE   "Reuse a dominating mask of the same pointer"       OFF)
option(OPT_KNOWNBITS "Skip stores whose bit 47 is known to be zero"      OFF)
option(OPTS_ALL      "Enable all the optimizations above"                OFF)
option(OPT_INPLACE   "Mask the base pointer once, keep constant offsets" OFF)

foreach(opt CONSTADDR DOMTREE KNOWNBITS INPLACE)
    if(OPTS_ALL OR OPT_${opt})
        target_compile_definitions(ShadowStack PRIVATE ${opt}_OPT)
        list(APPEND enabled_opts ${opt})
    endif()
endforeach()

option(MASK_BTR_ASM "Mask with btrq $47 (inline asm) instead of llvm.ptrmask" OFF)
if(MASK_BTR_ASM)
    target_compile_definitions(ShadowStack PRIVATE BTR_ASM)
    message(STATUS "Mask instruction: btrq")
else()
    message(STATUS "Mask instruction: ptrmask")
endif()

message(STATUS "Shadow stack optimizations: ${enabled_opts}")