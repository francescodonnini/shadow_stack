#include "ShadowStack.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/KnownBits.h"

#define DEBUG_TYPE "shadow-stack-opt"

using namespace llvm;

STATISTIC(NumStoresSeen, "Total number of stores evaluated");
STATISTIC(NumSkippedKnownBits, "Number of masks skipped (Known Bits)");
STATISTIC(NumSkippedSCEV, "Number of masks skipped (Scalar Evolution)");
STATISTIC(NumSkippedDomTree, "Number of masks skipped (Dominator Tree)");
STATISTIC(NumStoresInstrumented, "Number of masks actually injected");

static constexpr unsigned GuardBit = 47;
static constexpr uint64_t ClearMask = ~(1ULL << GuardBit);

PreservedAnalyses SSFunctionPass::run(Function &F,
                                      FunctionAnalysisManager &FAM) {
  if (F.isDeclaration()) {
    return PreservedAnalyses::all();
  }

  if (_instrumentStore) {
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
    instrumentStores(F, DT, SE);
  }

  return PreservedAnalyses::none();
}

void SSFunctionPass::instrumentStores(Function &F, DominatorTree &DT,
                                      ScalarEvolution &SE) {
  SmallVector<StoreInst *, 8> Stores;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (!SI->getMetadata("m-ignore")) {
          Stores.push_back(SI);
        }
      }
    }
  }

  auto &EntryBB = F.getEntryBlock();
  IRBuilder<> Builder(&EntryBB, EntryBB.getFirstNonPHIIt());

  DenseMap<Value *, Value *> MaskedPtrs;
  for (auto SI : Stores) {
    instrumentStore(F, *SI, DT, SE, MaskedPtrs);
  }
}

void SSFunctionPass::instrumentStore(Function &F, StoreInst &I,
                                     DominatorTree &DT, ScalarEvolution &SE,
                                     DenseMap<Value *, Value *> &MaskedPtrs) {
  NumStoresSeen++;
#if defined(OPT_ALL) || defined(KNOWNBITS_OPT)
  if (knownBitsOpt(F, I, Mask, DT, SE))
    return;
#endif

#if defined(OPT_ALL) || defined(SCEV_OPT)
  if (scalarEvolutionOpt(I, Mask, DT, SE))
    return;
#endif

#if defined(OPT_ALL) || defined(DOMTREE_OPT)
  if (domTreeOpt(I, DT, MaskedPtrs))
    return;
#endif

  IRBuilder<> Builder(&I);
  auto *M = F.getParent();
  auto *I64Ty = Builder.getInt64Ty();
  auto *I64PtrTy = PointerType::getUnqual(I64Ty);
  auto *DstPtr = I.getPointerOperand();
  auto *PtrTy = DstPtr->getType();
  auto *IdxTy = F.getParent()->getDataLayout().getIndexType(PtrTy);
  auto *DstI64 = Builder.CreatePtrToInt(DstPtr, Builder.getInt64Ty());
  auto *MaskedPtr = Builder.CreateIntrinsic(
      Intrinsic::ptrmask, {PtrTy, IdxTy},
      {DstPtr, ConstantInt::get(IdxTy, ClearMask)}, nullptr, "masked_adr");
  I.setOperand(I.getPointerOperandIndex(), MaskedPtr);
  MaskedPtrs[DstPtr] = MaskedPtr;
  NumStoresInstrumented++;
}

bool SSFunctionPass::knownBitsOpt(Function &F, StoreInst &I, uint64_t Mask,
                                  DominatorTree &DT, ScalarEvolution &SE) {
  const auto &DL = F.getParent()->getDataLayout();
  auto Known = computeKnownBits(I.getPointerOperand(), DL);
  auto BitsToClear = ~Mask;
  if (Known.Zero[GuardBit]) {
    NumSkippedKnownBits++;
    return true;
  }

  return false;
}

bool SSFunctionPass::scalarEvolutionOpt(StoreInst &I, uint64_t Mask,
                                        DominatorTree &DT,
                                        ScalarEvolution &SE) {
  auto Range = SE.getUnsignedRange(SE.getSCEV(I.getPointerOperand()));
  auto Max = Range.getUnsignedMax();
  if (Max.ult(APInt::getOneBitSet(Max.getBitWidth(), GuardBit))) {
    NumSkippedSCEV++;
    return true;
  }

  return false;
}

bool SSFunctionPass::domTreeOpt(StoreInst &I, DominatorTree &DT,
                                DenseMap<Value *, Value *> &MaskedPtrs) {
  auto *DstPtr = I.getPointerOperand();
  if (MaskedPtrs.count(DstPtr)) {
    auto *PrevMaskedPtr = MaskedPtrs[DstPtr];
    auto *PrevInst = dyn_cast<Instruction>(PrevMaskedPtr);
    if (!PrevInst || DT.dominates(PrevInst, &I)) {
      I.setOperand(I.getPointerOperandIndex(), PrevMaskedPtr);
      NumSkippedDomTree++;
      return true;
    }
  }

  return false;
}

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ShadowStack", "v0.1", [](PassBuilder &PB) {
            PB.registerOptimizerLastEPCallback([](ModulePassManager &MPM,
                                                  OptimizationLevel OL,
                                                  ThinOrFullLTOPhase) {
              MPM.addPass(createModuleToFunctionPassAdaptor(SSFunctionPass()));
            });
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "shadow-stack") {
                    // Add the pass when invoked manually via
                    // -passes="shadow-stack"
                    MPM.addPass(
                        createModuleToFunctionPassAdaptor(SSFunctionPass()));
                    return true;
                  }
                  return false;
                });
          }};
}