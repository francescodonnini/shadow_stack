#include "ShadowStack.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/KnownBits.h"
#include <cstdlib>

#define DEBUG_TYPE "shadow-stack-opt"

using namespace llvm;

STATISTIC(NumFunctions, "Number of functions with at least one store");
STATISTIC(NumStoresSeen, "Total number of stores evaluated");
STATISTIC(NumSkippedKnownBits, "Number of masks skipped (Known Bits)");
STATISTIC(NumSkippedDomTree, "Number of masks skipped (Dominator Tree)");
STATISTIC(NumSkippedConstAddr,
          "Number of masks skipped (constant offset in a local or global)");
STATISTIC(NumStoresInstrumented, "Number of masks actually injected");

static constexpr unsigned GuardBit = 47;
static constexpr uint64_t ClearMask = ~(1ULL << GuardBit);

static bool domTreeOpt(StoreInst &I, DominatorTree &DT,
                       DenseMap<Value *, Value *> &MaskedPtrs) {
  auto *DstPtr = I.getPointerOperand();
  if (MaskedPtrs.count(DstPtr)) {
    auto *PrevMaskedPtr = MaskedPtrs[DstPtr];
    auto *PrevInst = dyn_cast<Instruction>(PrevMaskedPtr);
    if (!PrevInst || DT.dominates(PrevInst, &I)) {
      I.setOperand(I.getPointerOperandIndex(), PrevMaskedPtr);
      return true;
    }
  }

  return false;
}

static bool isConstantAddressInObject(StoreInst &I, const DataLayout &DL) {
  auto *Ptr = I.getPointerOperand();
  APInt Off(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
  auto *Base = Ptr->stripAndAccumulateConstantOffsets(DL, Off, true);
  uint64_t ObjSize;
  if (auto *AI = dyn_cast<AllocaInst>(Base)) {
    auto Size = AI->getAllocationSize(DL);
    if (!Size || Size->isScalable()) {
      return false;
    }
    ObjSize = Size->getFixedValue();
  } else if (auto *GV = dyn_cast<GlobalVariable>(Base)) {
    if (GV->isThreadLocal()) {
      return false;
    }
    ObjSize = DL.getTypeAllocSize(GV->getValueType()).getFixedValue();
  } else {
    return false;
  }

  auto StoreSize = DL.getTypeStoreSize(I.getValueOperand()->getType());
  if (StoreSize.isScalable() || Off.isNegative()) {
    return false;
  }

  return Off.getZExtValue() + StoreSize.getFixedValue() <= ObjSize;
}

static bool isGuardBitKnownZero(StoreInst &I, const DataLayout &DL,
                                DominatorTree &DT) {
  auto Known = computeKnownBits(I.getPointerOperand(), DL, 0, nullptr, &I, &DT);
  return Known.Zero[GuardBit];
}

PreservedAnalyses SSFunctionPass::run(Function &F,
                                      FunctionAnalysisManager &FAM) {
  if (F.isDeclaration()) {
    return PreservedAnalyses::all();
  }

  if (_instrumentStore) {
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    instrumentStores(F, DT);
  }

  return PreservedAnalyses::none();
}

void SSFunctionPass::instrumentStores(Function &F, DominatorTree &DT) {
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

  if (!Stores.empty()) {
    NumFunctions++;
  }

  DenseMap<Value *, Value *> MaskedPtrs;
  for (auto SI : Stores) {
    instrumentStore(F, *SI, DT, MaskedPtrs);
  }
}

void SSFunctionPass::instrumentStore(Function &F, StoreInst &I,
                                     DominatorTree &DT,
                                     DenseMap<Value *, Value *> &MaskedPtrs) {
  NumStoresSeen++;
#if defined(CONSTADDR_OPT)
  if (isConstantAddressInObject(I, F.getParent()->getDataLayout())) {
    NumSkippedConstAddr++;
    return;
  }
#endif
#if defined(KNOWNBITS_OPT)
  if (isGuardBitKnownZero(I, F.getParent()->getDataLayout(), DT)) {
    NumSkippedKnownBits++;
    return;
  }
#endif

#if defined(DOMTREE_OPT)
  if (domTreeOpt(I, DT, MaskedPtrs)) {
    NumSkippedDomTree++;
    return;
  }
#endif

  IRBuilder<> Builder(&I);
  auto *DstPtr = I.getPointerOperand();
  auto *PtrTy = DstPtr->getType();
  auto *IdxTy = F.getParent()->getDataLayout().getIndexType(PtrTy);
  auto *MaskedPtr = Builder.CreateIntrinsic(
      Intrinsic::ptrmask, {PtrTy, IdxTy},
      {DstPtr, ConstantInt::get(IdxTy, ClearMask)}, nullptr, "masked_adr");
  I.setOperand(I.getPointerOperandIndex(), MaskedPtr);
  MaskedPtrs[DstPtr] = MaskedPtr;
  NumStoresInstrumented++;
}

PreservedAnalyses SSReportPass::run(Module &M, ModuleAnalysisManager &MAM) {
  static constexpr unsigned N = 6;
  static uint64_t Prev[N];
  uint64_t Now[N] = {NumFunctions,          NumStoresSeen,
                     NumStoresInstrumented, NumSkippedConstAddr,
                     NumSkippedKnownBits,   NumSkippedDomTree};
  uint64_t D[N];

  for (unsigned i = 0; i < N; ++i) {
    D[i] = Now[i] - Prev[i];
    Prev[i] = Now[i];
  }

  const auto Dest = std::getenv("SS_REPORT_DEST");
  if (!Dest || !*Dest) {
    return PreservedAnalyses::all();
  }

  if (StringRef(Dest) == "-") {
    errs() << "[ss] " << M.getSourceFileName() << ": " << D[0] << " functions, "
           << D[1] << " stores, " << D[2] << " masked (skipped: constaddr "
           << D[3] << ", knownbits " << D[4] << ", domtree " << D[5] << ")\n";
    return PreservedAnalyses::all();
  }

  std::error_code EC;
  raw_fd_ostream Out(Dest, EC, sys::fs::OF_Append | sys::fs::OF_Text);
  if (EC) {
    errs() << "[ss] cannot open " << Dest << ": " << EC.message() << "\n";
    return PreservedAnalyses::all();
  }

  std::string Line;
  raw_string_ostream L(Line);
  L << M.getSourceFileName();
  for (unsigned i = 0; i < N; i++) {
    L << "," << D[i];
  }
  L << "\n";
  Out << L.str();
  return PreservedAnalyses::all();
}

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ShadowStack", "v0.1", [](PassBuilder &PB) {
            PB.registerOptimizerLastEPCallback([](ModulePassManager &MPM,
                                                  OptimizationLevel OL,
                                                  ThinOrFullLTOPhase) {
              MPM.addPass(createModuleToFunctionPassAdaptor(SSFunctionPass()));
              MPM.addPass(SSReportPass());
            });
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "shadow-stack") {
                    // Add the pass when invoked manually via
                    // -passes="shadow-stack"
                    MPM.addPass(
                        createModuleToFunctionPassAdaptor(SSFunctionPass()));
                    MPM.addPass(SSReportPass());
                    return true;
                  }
                  return false;
                });
          }};
}