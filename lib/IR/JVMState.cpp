#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/JVMState.h"

using namespace llvm;

#define VM_STATE_FUNCTION_NAME "llvm.jvmstate_"
#define VM_STATE_ANCHOR_NAME "llvm.jvmstate_anchor"

bool llvm::isJVMState(const Value *V) {
  if (const CallInst *CI = dyn_cast<CallInst>(V)) {
    const Function *F = CI->getCalledFunction();
    return (F && F->getName().startswith(VM_STATE_FUNCTION_NAME));
  }
  return false;
}

bool llvm::isJVMStateAnchorInstruction(const Value *V) {
  if (const StoreInst *SI = dyn_cast<StoreInst>(V)) {
    if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(SI->getPointerOperand())) {
      return GV->getName().startswith(VM_STATE_ANCHOR_NAME);
    }
  }
  return false;
}

#ifndef NDEBUG
void llvm::assertJVMStateSanity(const CallInst *vmState) {
  // getSExtValue asserts if the value doens't fit in 64 bits, so we
  // won't get a silent overflow here.
  int64_t numStack = cast<ConstantInt>(vmState->getArgOperand(JVMState::numStackElementsIndex))->getSExtValue();
  int64_t numLocals = cast<ConstantInt>(vmState->getArgOperand(JVMState::numLocalsIndex))->getSExtValue();
  int64_t numMons = cast<ConstantInt>(vmState->getArgOperand(JVMState::numMonitorIndex))->getSExtValue();

  assert((2 * numStack + numMons + 2 * numLocals) == (vmState->getNumArgOperands() - JVMState::headerEndOffset()) &&
         "invalid vm state!");

  for (int i = JVMState::headerEndOffset(); i < JVMState::headerEndOffset() + 2 * numStack + 2 * numLocals; i += 2) {
    // These values are type tags and *have* to remain constant
    // integers for correctness!
    cast<ConstantInt>(vmState->getArgOperand(i));
  }
}
#endif
