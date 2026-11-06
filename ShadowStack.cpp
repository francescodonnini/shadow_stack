#include "ShadowStack.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/KnownBits.h"
#include <cstdlib>
#include <iterator>
#include <optional>
#include <type_traits>

#define DEBUG_TYPE "shadow-stack-opt"

using namespace llvm;

STATISTIC(NumFunctions, "Number of functions with at least one store to mask");
STATISTIC(NumStoresSeen, "Total number of stores evaluated");
STATISTIC(NumStoresInstrumented, "Number of masks actually injected");
STATISTIC(NumStoreInst, "Number of simple stores");
STATISTIC(NumAtomicRMW, "Number of atomic RMW");
STATISTIC(NumCmpXchg, "Number of cmp-xchg");
STATISTIC(NumMemIntrinsics, "Number of LLVM memory intrinsics");
STATISTIC(NumMaskedStore, "Number of llvm.masked.store calls");
STATISTIC(NumScatter, "Number of llvm.masked.scatter calls");
STATISTIC(NumVectorStores, "Number of stores of vector type");
STATISTIC(NumArchSpecificStores, "Number of x86 maskmov/maskstore intrinsics");
STATISTIC(NumUnhandledWrites,
          "Number of arch-specific intrinsics not recognized by LLVM");
STATISTIC(NumSkippedAlreadyMasked,
          "Number of stores already mask and processed two or more times");
STATISTIC(NumInlineAsm, "Number of inline asm code sections");
STATISTIC(NumExternalCallsMayWrite,
          "Number of function call not defined in the module");
STATISTIC(NumSkippedKnownBits, "Number of masks skipped (Known Bits)");
STATISTIC(NumSkippedDomTree, "Number of masks skipped (Dominator Tree)");
STATISTIC(NumSkippedConstAddr,
          "Number of masks skipped (constant offset in a local or global)");
STATISTIC(NumBaseSmallOff, "Stores at base + small constant offset");
STATISTIC(NumDistinctBases, "Distinct bases of those stores (per function)");
STATISTIC(NumNonZeroOff, "Stores at base + nonzero small constant offset");
STATISTIC(NumInPlace, "Stores masked through a shared base mask");

namespace {
using TStat = std::remove_reference_t<decltype(NumFunctions)>;
struct ColumnValue {
  const char *Name;
  const TStat *Stat;
};

enum ColumnIndex : size_t {
  IdxFunctions,
  IdxSeen,
  IdxInstrumented,
  IdxAlreadyMasked,
  IdxSkipConstAddr,
  IdxSkipKnownBits,
  IdxSkipDomTree,
  IdxStoreInst,
  IdxVectorStores,
  IdxAtomicRMW,
  IdxCmpXchg,
  IdxMemIntrinsics,
  IdxMaskedStore,
  IdxScatter,
  IdxArchSpecificStores,
  IdxUnhandledWrites,
  IdxInlineAsm,
  IdxExternalCalls,
  IdxBaseSmallOff,
  IdxDistinctBases,
  IdxNonZeroOff,
  IdxInPlace,
  IdxNum
};
} // namespace

static const ColumnValue Columns[] = {
    {"functions", &NumFunctions},
    {"stores_seen", &NumStoresSeen},
    {"instrumented", &NumStoresInstrumented},
    {"already_masked", &NumSkippedAlreadyMasked},
    {"skipped_constaddr", &NumSkippedConstAddr},
    {"skipped_knownbits", &NumSkippedKnownBits},
    {"skipped_domtree", &NumSkippedDomTree},
    {"store_inst", &NumStoreInst},
    {"vector_stores", &NumVectorStores},
    {"atomic_rmw", &NumAtomicRMW},
    {"cmpxchg", &NumCmpXchg},
    {"mem_intrinsics", &NumMemIntrinsics},
    {"masked_store", &NumMaskedStore},
    {"arch_specific", &NumArchSpecificStores},
    {"scatter", &NumScatter},
    {"unhandled_writes", &NumUnhandledWrites},
    {"inline_asm", &NumInlineAsm},
    {"external_calls_may_write", &NumExternalCallsMayWrite},
    {"base_small_off", &NumBaseSmallOff},
    {"distinct_bases", &NumDistinctBases},
    {"non_zero_off", &NumNonZeroOff},
    {"inplace", &NumInPlace},
};
static constexpr size_t NumColumns = std::size(Columns);
static_assert(NumColumns == IdxNum,
              "Columns array is out of sync with ColumnIndex enum");

static constexpr unsigned GuardBit = 47;
static constexpr uint64_t ClearMask = ~(1ULL << GuardBit);

static bool domTreeOpt(Use &U, DominatorTree &DT,
                       DenseMap<Value *, Value *> &MaskedPtrs) {
  auto It = MaskedPtrs.find(U.get());
  if (It == MaskedPtrs.end()) {
    return false;
  }

  auto *Prev = dyn_cast<Instruction>(It->second);
  if (Prev && !DT.dominates(Prev, cast<Instruction>(U.getUser()))) {
    return false;
  }

  U.set(It->second);
  return true;
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

static bool isGuardBitKnownZero(Use &U, const DataLayout &DL,
                                DominatorTree &DT) {
  auto *I = cast<Instruction>(U.getUser());
  return computeKnownBits(U.get(), DL, 0, nullptr, I, &DT).Zero[GuardBit];
}

PreservedAnalyses SSFunctionPass::run(Function &F,
                                      FunctionAnalysisManager &FAM) {
  if (F.isDeclaration()) {
    return PreservedAnalyses::all();
  }

  auto Changed = false;
  if (_instrumentStore) {
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    Changed = instrumentStores(F, DT);
  }

  if (!Changed) {
    return PreservedAnalyses::all();
  }

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

static std::optional<int> x86MaskedWritePtrIdx(Intrinsic::ID &ID) {
  switch (ID) {
  case Intrinsic::x86_sse2_maskmov_dqu:
  case Intrinsic::x86_mmx_maskmovq:
    return std::optional{2};
  case Intrinsic::x86_avx_maskstore_pd:
  case Intrinsic::x86_avx_maskstore_ps:
  case Intrinsic::x86_avx_maskstore_pd_256:
  case Intrinsic::x86_avx_maskstore_ps_256:
  case Intrinsic::x86_avx2_maskstore_d:
  case Intrinsic::x86_avx2_maskstore_q:
  case Intrinsic::x86_avx2_maskstore_d_256:
  case Intrinsic::x86_avx2_maskstore_q_256:
    return std::optional{0};
  default:
    return std::nullopt;
  }
}

static std::optional<Use *> getUse(Instruction &I) {
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    return std::optional{
        &SI->getOperandUse(StoreInst::getPointerOperandIndex())};
  }

  if (isa<AtomicRMWInst>(&I)) {
    return std::optional{
        &I.getOperandUse(AtomicRMWInst::getPointerOperandIndex())};
  }

  if (isa<AtomicCmpXchgInst>(&I)) {
    return std::optional{
        &I.getOperandUse(AtomicCmpXchgInst::getPointerOperandIndex())};
  }

  // memset/memcpy/memmove
  if (auto *MI = dyn_cast<MemIntrinsic>(&I)) {
    return std::optional{&MI->getRawDestUse()};
  }

  if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
    auto ID = II->getIntrinsicID();
    switch (ID) {
    case Intrinsic::masked_store:
      return std::optional{&II->getArgOperandUse(1)};
    case Intrinsic::masked_scatter:
      return std::optional{&II->getArgOperandUse(1)};
      break;
    default:
      if (auto Idx = x86MaskedWritePtrIdx(ID)) {
        return std::optional{&II->getArgOperandUse(*Idx)};
      }
    }
  }

  return std::nullopt;
}

static void countType(Instruction &I) {
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    NumStoreInst++;
    if (SI->getValueOperand()->getType()->isVectorTy()) {
      NumVectorStores++;
    }
  }

  if (isa<AtomicRMWInst>(&I)) {
    NumAtomicRMW++;
  }

  if (isa<AtomicCmpXchgInst>(&I)) {
    NumCmpXchg++;
  }

  // memset/memcpy/memmove
  if (isa<MemIntrinsic>(&I)) {
    NumMemIntrinsics++;
  }

  if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
    auto ID = II->getIntrinsicID();
    switch (ID) {
    case Intrinsic::masked_store:
      NumMaskedStore++;
      break;
    case Intrinsic::masked_scatter:
      NumScatter++;
      break;
    default:
      if (x86MaskedWritePtrIdx(ID)) {
        NumArchSpecificStores++;
      }
      break;
    }
  }
}

static bool isHarmlessIntrinsic(Intrinsic::ID ID) {
  switch (ID) {
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::invariant_start:
  case Intrinsic::invariant_end:
  case Intrinsic::assume:
  case Intrinsic::experimental_noalias_scope_decl:
  case Intrinsic::sideeffect:
    return true;
  default:
    return false;
  }
}

static void countUnhandled(Instruction &I) {
  if (!I.mayWriteToMemory()) {
    return;
  }

  auto *CB = dyn_cast<CallBase>(&I);
  if (!CB) {
    return;
  }

  if (CB->isInlineAsm()) {
    NumInlineAsm++;
  } else if (auto *II = dyn_cast<IntrinsicInst>(CB)) {
    if (isHarmlessIntrinsic(II->getIntrinsicID())) {
      return;
    }

    NumUnhandledWrites++;
    static StringSet<> Warned;
    auto Name = II->getCalledFunction()->getName();
    if (Warned.insert(Name).second) {
      errs() << "WARNING: unhandled memory-writing intrinsic " << Name
             << " (first seen in " << I.getFunction()->getName() << ")\n";
    }
  } else if (auto *F = CB->getCalledFunction(); !F || F->isDeclaration()) {
    NumExternalCallsMayWrite++;
  }
}

static bool isAlreadyMasked(Value *P) {
  auto *II = dyn_cast<IntrinsicInst>(P);
  if (!II || II->getIntrinsicID() != Intrinsic::ptrmask) {
    return false;
  }

  auto *C = dyn_cast<Constant>(II->getArgOperand(1));
  if (!C) {
    return false;
  }

  if (C->getType()->isVectorTy()) {
    C = C->getSplatValue();
  }

  auto *CI = dyn_cast_or_null<ConstantInt>(C);
  return CI && CI->getValue() == ClearMask;
}

static const Value *smallOffsetBase(Use &U, const DataLayout &DL) {
  auto *Ptr = U.get();
  if (!Ptr->getType()->isPointerTy()) // es. vettori di puntatori (scatter)
    return nullptr;
  APInt Off(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
  auto *Base = Ptr->stripAndAccumulateConstantOffsets(DL, Off, true);
  if (Off.abs().uge(4096))
    return nullptr;
  if (!Off.isZero())
    NumNonZeroOff++;
  return Base;
}

bool SSFunctionPass::instrumentStores(Function &F, DominatorTree &DT) {
  BaseMasks.clear();
  SmallPtrSet<const Value *, 16> Bases;
  SmallVector<Use *, 8> Stores;
  auto &DL = F.getParent()->getDataLayout();
  for (auto &BB : F) {
    for (auto &I : BB) {
      auto o = getUse(I);
      if (o.has_value()) {
        auto U = o.value();
        NumStoresSeen++;
        if (isAlreadyMasked(U->get())) {
          NumSkippedAlreadyMasked++;
        } else {
          countType(I);
          Stores.push_back(U);
          if (auto *B = smallOffsetBase(*U, DL)) {
            NumBaseSmallOff++;
            Bases.insert(B);
          }
        }
      } else {
        countUnhandled(I);
      }
    }
  }

  if (Stores.empty()) {
    return false;
  }

  NumFunctions++;
  auto Changed = false;
  DenseMap<Value *, Value *> MaskedPtrs;
  for (auto S : Stores) {
    Changed |= instrumentStore(F, *S, DT, MaskedPtrs);
  }

  NumDistinctBases += Bases.size();
  return Changed;
}

#if defined(INPLACE_OPT)
static Value *maskedBaseFor(Value *Base, Function &F,
                            DenseMap<Value *, Value *> &Cache) {
  if (auto It = Cache.find(Base); It != Cache.end())
    return It->second;

  BasicBlock::iterator IP;
  if (auto *I = dyn_cast<Instruction>(Base)) {
    if (I->isTerminator()) {
      return nullptr;
    }
    IP = I->getParent()->getFirstInsertionPt();
    if (!isa<PHINode>(I))
      IP = std::next(I->getIterator());
  } else {
    IP = F.getEntryBlock().getFirstInsertionPt();
  }

  IRBuilder<> B(&*IP);
  auto *IdxTy = F.getParent()->getDataLayout().getIndexType(Base->getType());
  auto *M = B.CreateIntrinsic(Intrinsic::ptrmask, {Base->getType(), IdxTy},
                              {Base, ConstantInt::get(IdxTy, ClearMask)},
                              nullptr, "masked_base");
  return Cache[Base] = M;
}
#endif

bool SSFunctionPass::instrumentStore(Function &F, Use &U, DominatorTree &DT,
                                     DenseMap<Value *, Value *> &MaskedPtrs) {
  auto &I = *cast<Instruction>(U.getUser());

#if defined(CONSTADDR_OPT)
  if (auto *SI = dyn_cast<StoreInst>(&I);
      SI && isConstantAddressInObject(*SI, F.getParent()->getDataLayout())) {
    NumSkippedConstAddr++;
    return false;
  }
#endif
#if defined(KNOWNBITS_OPT)
  if (isGuardBitKnownZero(U, F.getParent()->getDataLayout(), DT)) {
    NumSkippedKnownBits++;
    return false;
  }
#endif
#if defined(DOMTREE_OPT)
  if (domTreeOpt(U, DT, MaskedPtrs)) {
    NumSkippedDomTree++;
    return true;
  }
#endif

#if defined(INPLACE_OPT)
  {
    auto &DL = F.getParent()->getDataLayout();
    auto *Ptr = U.get();
    if (Ptr->getType()->isPointerTy()) {
      APInt Off(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
      auto *Base = Ptr->stripAndAccumulateConstantOffsets(DL, Off, true);
      if (Off.abs().ult(4096)) {
        if (auto *MB = maskedBaseFor(Base, F, BaseMasks)) {
          IRBuilder<> B(&I);
          U.set(
              B.CreatePtrAdd(MB, B.getInt64(Off.getSExtValue()), "masked_adr"));
          NumInPlace++;
          return true;
        }
      }
    }
  }
#endif

  IRBuilder<> Builder(&I);
  auto *DstPtr = U.get();
  auto *PtrTy = DstPtr->getType();
  auto *IdxTy = F.getParent()->getDataLayout().getIndexType(PtrTy);
  auto *MaskedPtr = Builder.CreateIntrinsic(
      Intrinsic::ptrmask, {PtrTy, IdxTy},
      {DstPtr, ConstantInt::get(IdxTy, ClearMask)}, nullptr, "masked_adr");
  U.set(MaskedPtr);
  MaskedPtrs[DstPtr] = MaskedPtr;
  NumStoresInstrumented++;
  return true;
}

PreservedAnalyses SSReportPass::run(Module &M, ModuleAnalysisManager &MAM) {
  static uint64_t Prev[NumColumns];
  uint64_t D[NumColumns];
  for (size_t i = 0; i < NumColumns; ++i) {
    uint64_t Now = Columns[i].Stat->getValue();
    D[i] = Now - Prev[i];
    Prev[i] = Now;
  }

  uint64_t Accounted = D[IdxAlreadyMasked] + D[IdxInstrumented] +
                       D[IdxSkipConstAddr] + D[IdxSkipKnownBits] +
                       D[IdxSkipDomTree] + D[IdxInPlace];
  if (Accounted != D[IdxSeen]) {
    errs() << "WARNING: " << M.getSourceFileName() << ": seen=" << D[IdxSeen]
           << " accounted=" << Accounted << "\n";
  }

  const auto Dest = std::getenv("SS_REPORT_DEST");
  if (!Dest || !*Dest) {
    return PreservedAnalyses::all();
  }

  if (StringRef(Dest) == "-") {
    errs() << M.getSourceFileName();
    for (size_t i = 0; i < NumColumns; ++i) {
      errs() << (i ? "," : ":") << Columns[i].Name << "=" << D[i];
    }

    errs() << "\n";
    return PreservedAnalyses::all();
  }

  std::error_code EC;
  raw_fd_ostream Out(Dest, EC, sys::fs::OF_Append | sys::fs::OF_Text);
  if (EC) {
    errs() << "WARNING: cannot open " << Dest << ": " << EC.message() << "\n";
    return PreservedAnalyses::all();
  }

  std::string Line;
  raw_string_ostream L(Line);
  L << M.getSourceFileName();
  for (unsigned i = 0; i < NumColumns; i++) {
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
                                                  ThinOrFullLTOPhase Phase) {
              if (Phase == ThinOrFullLTOPhase::FullLTOPreLink ||
                  Phase == ThinOrFullLTOPhase::ThinLTOPreLink)
                return;

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