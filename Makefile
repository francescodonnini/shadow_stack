-include local.mk

LLVM_CONFIG     ?= llvm-config
LLVM_BIN        =  $(shell $(LLVM_CONFIG) --bindir)
LLVM_LIB        =  $(shell $(LLVM_CONFIG) --libdir)
LLVM_BUILD_ROOT =  $(shell $(LLVM_CONFIG) --obj-root)
LLVM_SRC_ROOT   =  $(LLVM_BUILD_ROOT)/../llvm
LLVM_CMAKE_DIR  =  $(LLVM_BUILD_DIR)/lib/cmake/llvm
CC              =  $(LLVM_BIN)/clang
CXX             =  $(LLVM_BIN)/clang++
OPT             =  $(LLVM_BIN)/opt
LLC             =  $(LLVM_BIN)/llc
LLINK           =  $(LLVM_BIN)/llvm-link
OML             =  -O3

all:
	mkdir -p build
	cd build && cmake -DLLVM_DIR=$(LLVM_CMAKE_DIR) \
		-DCMAKE_CXX_COMPILER=$(CXX) \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		..
	$(MAKE) -C build

compdb:
	bear -- $(MAKE) -B all

clean:
	rm -rf build