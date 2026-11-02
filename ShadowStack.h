#ifndef STRICT_OPT_H
#define STRICT_OPT_H
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;

struct SSFunctionPass : public PassInfoMixin<SSFunctionPass> {
  SSFunctionPass(bool instrumentFun = true, bool instrumentStore = true)
      : _instrumentFun(instrumentFun), _instrumentStore(instrumentStore) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

private:
  bool _instrumentFun;
  bool _instrumentStore;

  void instrumentStores(Function &F, DominatorTree &DT);

  void instrumentStore(Function &F, StoreInst &I, DominatorTree &DT,
                       DenseMap<Value *, Value *> &MaskedPtrs);
};

struct SSReportPass : public PassInfoMixin<SSReportPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
} // namespace llvm
#endif