#ifndef _SHADOW_STACK_H
#define _SHADOW_STACK_H
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;

struct SSFunctionPass : public PassInfoMixin<SSFunctionPass> {
  SSFunctionPass(bool instrumentStore = true)
      : _instrumentStore(instrumentStore) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

private:
  DenseMap<Value *, Value *> BaseMasks;

  bool _instrumentStore;

  bool instrumentStores(Function &F, DominatorTree &DT);

  bool instrumentStore(Function &F, Use &U, DominatorTree &DT,
                       DenseMap<Value *, Value *> &MaskedPtrs);
};

struct SSReportPass : public PassInfoMixin<SSReportPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
} // namespace llvm
#endif