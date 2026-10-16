-include ../local.mk

LLVM_BUILD_DIR = $(shell $(LLVM_CONFIG) --obj-root)
LLVM_BIN       = $(shell $(LLVM_CONFIG) --bindir)
LLVM_CMAKE_DIR = $(LLVM_BUILD_DIR)/lib/cmake/llvm
CC             = clang
CXX            = clang++
OPT            = $(LLVM_BIN)/opt
LLC            = $(LLVM_BIN)/llc
LLINK          = $(LLVM_BIN)/llvm-link
OML            = -O3

all:
	mkdir -p build
	cd build && cmake -DLLVM_DIR=$(LLVM_CMAKE_DIR) \
		-DCMAKE_CXX_COMPILER=$(CXX) \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		..
	$(MAKE) -C build

clean:
	rm -rf build