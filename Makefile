-include local.mk

LLVM_BIN        = $(shell $(LLVM_CONFIG) --bindir)
LLVM_LIB        = $(shell $(LLVM_CONFIG) --libdir)
LLVM_BUILD_ROOT = $(shell $(LLVM_CONFIG) --obj-root)
LLVM_CMAKE_DIR  = $(shell $(LLVM_CONFIG) --cmakedir)
LLVM_SRC_ROOT   = $(LLVM_BUILD_ROOT)/../llvm
CC              = $(LLVM_BIN)/clang
CXX             = $(LLVM_BIN)/clang++
OPT             = $(LLVM_BIN)/opt
LLC             = $(LLVM_BIN)/llc
LLINK           = $(LLVM_BIN)/llvm-link
OML             = -O3

# Due varianti del mascheramento, ognuna con la sua cartella di build (così la
# cache di cmake non mescola le opzioni):
#   make ptrm   llvm.ptrmask  -> movabs + and   (default, anche con "make")
#   make btrq   inline asm    -> btrq $47
# Il plugin compilato finisce in build/ShadowStack-<variante>.so, e
# build/ShadowStack.so (il percorso usato dagli script di build) diventa un link
# all'ultima variante compilata: ls -l build/ShadowStack.so dice quale.
# Altre opzioni di cmake: make btrq CMAKE_OPTS="-DOPT_DOMTREE=OFF"
VARIANTS        = ptrm btrq
FLAGS_ptrm      = -DMASK_BTR_ASM=OFF
FLAGS_btrq      = -DMASK_BTR_ASM=ON

.PHONY: all $(VARIANTS) compdb clean

all: ptrm

$(VARIANTS):
	mkdir -p build-$@
	cd build-$@ && cmake -DLLVM_DIR=$(LLVM_CMAKE_DIR) \
		-DCMAKE_CXX_COMPILER=$(CXX) \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		$(FLAGS_$@) $(CMAKE_OPTS) \
		..
	$(MAKE) -C build-$@
	mkdir -p build
	cp "$$(find build-$@ -name ShadowStack.so | head -1)" build/ShadowStack-$@.so
	ln -sfn ShadowStack-$@.so build/ShadowStack.so
	@echo "build/ShadowStack.so -> ShadowStack-$@.so"

compdb:
	bear -- $(MAKE) -B all

clean:
	rm -rf build $(addprefix build-,$(VARIANTS))