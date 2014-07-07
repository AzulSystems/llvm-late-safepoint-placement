

#include "llvm/IR/Function.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/CommandLine.h"

#include "llvm/IR/Statepoint.h"

using namespace std;
using namespace llvm;

bool llvm::isStatepoint(const ImmutableCallSite &CS) {
  const Function *F = CS.getCalledFunction();
  if (F && F->getIntrinsicID() == Intrinsic::statepoint) {
    return true;
  }
  return false;
}
bool llvm::isStatepoint(const Instruction *inst) {
  if (isa<InvokeInst>(inst) || isa<CallInst>(inst)) {
    ImmutableCallSite CS(inst);
    return isStatepoint(CS);
  }
  return false;
}
bool llvm::isStatepoint(const Instruction &inst) { return isStatepoint(&inst); }

bool llvm::isGCRelocate(const ImmutableCallSite &CS) {
  if (CS.isCall()) {
    return isGCRelocate(CS.getInstruction());
  }
  return false;
}
bool llvm::isGCRelocate(const Instruction *inst) {
  if (const CallInst *call = dyn_cast<CallInst>(inst)) {
    if (const Function *F = call->getCalledFunction()) {
      return F->getIntrinsicID() == Intrinsic::gc_relocate;
    }
  }
  return false;
}

bool llvm::isGCResult(const ImmutableCallSite &CS) {
  if (CS.isCall()) {
    return isGCResult(CS.getInstruction());
  }
  return false;
}
bool llvm::isGCResult(const Instruction *inst) {
  if (const CallInst *call = cast<CallInst>(inst)) {
    if (Function *F = call->getCalledFunction()) {
      return (F->getIntrinsicID() == Intrinsic::gc_result_int ||
              F->getIntrinsicID() == Intrinsic::gc_result_float ||
              F->getIntrinsicID() == Intrinsic::gc_result_ptr);
    }
  }
  return false;
}

cl::opt<bool> AllFunctions("spp-all-functions", cl::init(false));

bool llvm::isGCPointerType(llvm::Type *Ty) {

  if (!AllFunctions) {
    if (PointerType *PType = dyn_cast<PointerType>(Ty)) {
      static const int GC_ADDR_SPACE = 1;
      return (PType->getAddressSpace() == GC_ADDR_SPACE);
    }
    return false;
  }

  // If we're doing C validation, fall back to our previous hacky
  // hueristics.  This should be better factored before public release.
  // Strategy/Policy pattern?

  if (!Ty->isPointerTy()) {
    // can't be an gc value, ignore
    return false;
  }
  if (cast<PointerType>(Ty)->getElementType()->isFunctionTy()) {
    // A function pointer is likely not a GC-able value.  We may need a
    // safepoint at the call site, but the function pointer itself does not
    // need relocated.
    return false;
  }
  if (cast<PointerType>(Ty)->getElementType()->isPointerTy() &&
      cast<PointerType>(cast<PointerType>(Ty)->getElementType())
          ->getElementType()
          ->isFunctionTy()) {
    // The slot a function pointer is stored in also doesn't need GC.  In
    // practice, these only appear in C-structures not GC heap-structures.  Note
    // this is specific to our runtime and is not generally applicable.
    return false;
  }
  // Note: A thread local pointer (i.e. basically an offset into the thread
  // structure) *can* point to an gc-able value.
  return true;
}
