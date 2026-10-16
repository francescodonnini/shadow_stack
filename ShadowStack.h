#ifndef STRICT_OPT_H
#define STRICT_OPT_H
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;

struct SSFunctionPass : public PassInfoMixin<SSFunctionPass> {
    SSFunctionPass(bool instrumentFun = true, bool instrumentStore = true)
        :_instrumentFun(instrumentFun), _instrumentStore(instrumentStore) {
            errs() << "[ss] Function Instrumentation: " << (instrumentFun ? "ON" : "OFF") << ", Store Instrumentation: " << (instrumentStore ? "ON" : "OFF") << "\n";
        }

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
private:
    constexpr static auto MASK = 0x7fffffffffffULL;

    bool _instrumentFun;
    bool _instrumentStore;

    void instrumentStores(Function &F, DominatorTree &DT, ScalarEvolution &SE);

    void instrumentStore(Function &F, StoreInst &I, uint64_t Mask, Value *MaskVal, DominatorTree &DT, ScalarEvolution &SE, DenseMap<Value*, Value*> &MaskedPtrs);
    
    bool knownBitsOpt(Function &F, StoreInst &I, uint64_t Mask, DominatorTree &DT, ScalarEvolution &SE);
    
    bool scalarEvolutionOpt(StoreInst &I, uint64_t Mask, DominatorTree &DT, ScalarEvolution &SE);
    
    bool domTreeOpt(StoreInst &I, DominatorTree &DT, DenseMap<Value*, Value*> &MaskedPtrs);
};
}
#endif