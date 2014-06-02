// TODO: copyright header

#define DEBUG_TYPE "remove-redundant-vm-states"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/JVMState.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;

namespace {

struct RemoveRedundantVMStates : public FunctionPass {
  static char ID;

  RemoveRedundantVMStates();

  virtual bool runOnFunction(Function &F);
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;

  // Take `vmStatesAvailable` from the set of unclobbered vm states
  // available before the execution of I to the set of vm states
  // available after the execution of I.  In practice, this either
  // clears `vmStatesAvailable` or adds one new vm state to it.
  void transferInstruction(
      Instruction *I, DenseSet<CallInst *> *vmStatesAvailable /* updates in place */);

  // `transferInstruction` composed for the list of instructions in a
  // basic block.
  void transferBlock(BasicBlock *BB, DenseSet<CallInst *> *vmStatesAvailable /* updates in place */) {
    for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
      transferInstruction(&*I, vmStatesAvailable);
    }
  }

  void meetPredecessors(BasicBlock *BB, Function *F,
                        const DenseMap<BasicBlock *, DenseSet<CallInst *> > &vmStatesAtExit,
                        DenseSet<CallInst *> *vmStatesAtEntry) {
    // Meet (intersect) the exit lattice values (set of available VM
    // states) corresponding to the predecessors to get the entry
    // state of the current basic block.  The entry block doesn't have
    // any predecessors so for it vmStatesAtEntry should always be TOP
    // -- the set of dominating VM states.  Conveniently, this set is
    // empty for the entry block and needs no extra handling.
    if (BB != &F->getEntryBlock()) {
      pred_iterator PI = pred_begin(BB), PE = pred_end(BB);
      assert(PI != PE && "we should not be seeing unreachable basic blocksk");

      DenseMap<BasicBlock *, DenseSet<CallInst *> >::const_iterator exitIterator = vmStatesAtExit.find(*PI);
      assert(exitIterator != vmStatesAtExit.end() && "must have a set for every basic block");
      *vmStatesAtEntry = exitIterator->second;

      for (++PI; PI != PE; ++PI) {
        exitIterator = vmStatesAtExit.find(*PI);
        assert(exitIterator != vmStatesAtExit.end() && "must have a set for every basic block");
        set_intersect(*vmStatesAtEntry, exitIterator->second);
      }
    }
  }

  bool canBeSafelyReplayed(const Instruction *I);
  void removeVMStateCalls(const DenseSet<CallInst *> &calls);
  void getVMStatesDominatingBB(BasicBlock *BB, DominatorTree *DT,
                               DenseSet<CallInst *> *dominatingVMStates);
};

}

RemoveRedundantVMStates::RemoveRedundantVMStates() : FunctionPass(ID) {
  initializeRemoveRedundantVMStatesPass(*PassRegistry::getPassRegistry());
}

void RemoveRedundantVMStates::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTreeWrapperPass>();

  AU.addPreserved<DominatorTreeWrapperPass>();
  AU.setPreservesCFG();
}

bool RemoveRedundantVMStates::runOnFunction(Function &F) {
  DominatorTree *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();

  // The algorithm
  //
  // RemoveRedundantVMStates uses a fairly standard optimistic forward
  // data flow analysis.  Elements of the lattice are sets of VM
  // states available throughout the function.  Meet is set
  // intersection.  TOP is the set of all the VM states that could
  // *possibly* be used at a specific point (hence the value of TOP
  // varies from basic block to basic block).  This "universe" of
  // possibly usable VM states is the set of VM states dominating the
  // basic block in context.
  //
  // This forward DFA is used to compute the set of VM states live at
  // exit of each basic block.  Every VM state that sees a non-empty
  // set of unclobbered VM states can be dropped.

  DenseMap<BasicBlock *, DenseSet<CallInst *> > vmStatesAvailableAtExit;

  for (Function::iterator BBI = F.begin(), BBE = F.end(); BBI != BBE; ++BBI) {
    DenseSet<CallInst *> dominatingVMStates;
    getVMStatesDominatingBB(BBI, DT, &dominatingVMStates);
    transferBlock(BBI, &dominatingVMStates);
    vmStatesAvailableAtExit[BBI] = dominatingVMStates;
  }

  int iterations = 0;

  DenseSet<BasicBlock *> workset;
  for (Function::iterator BBI = F.begin(), BBE = F.end(); BBI != BBE; ++BBI) {
    workset.insert(BBI);
  }

  while (!workset.empty()) {
    assert(iterations++ < (1 * 1000 * 1000) && "infinite loop?");

    BasicBlock *BB = *workset.begin();
    workset.erase(BB);

    DenseSet<CallInst *> vmStatesAvailable;
    meetPredecessors(BB, &F, vmStatesAvailableAtExit, &vmStatesAvailable);

    // Update vmStatesAvailable in place to the output state.
    transferBlock(BB, &vmStatesAvailable);

    bool changed = !set_equals(vmStatesAvailable, vmStatesAvailableAtExit[BB]);
    vmStatesAvailableAtExit[BB] = vmStatesAvailable;

    // Crucial to our progress guarantee: vmStatesAvailableAtExit[BB]
    // either throws out some elements or stays the same.
    assert(set_is_subset(vmStatesAvailable, vmStatesAvailableAtExit[BB]) &&
           "we're not monotonic!");

    if (changed) {
      for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; ++SI) {
        BasicBlock *SBBlock = *SI;
        workset.insert(SBBlock);
      }
    }
  }

  DenseSet<CallInst *> redundantVMStates;

  for (Function::iterator BBI = F.begin(), BBE = F.end(); BBI != BBE; ++BBI) {
    DenseSet<CallInst *> vmStatesAvailable;
    meetPredecessors(BBI, &F, vmStatesAvailableAtExit, &vmStatesAvailable);

    for (BasicBlock::iterator I = BBI->begin(), E = BBI->end(); I != E; ++I) {
      if (isJVMState(&*I) && !vmStatesAvailable.empty()) {
        redundantVMStates.insert(cast<CallInst>(I));
      }
      transferInstruction(&*I, &vmStatesAvailable);
    }

    assert(set_equals(vmStatesAvailable, vmStatesAvailableAtExit[BBI]) &&
           "this is not the fixed point you are looking for!");
  }

  removeVMStateCalls(redundantVMStates);
  return !redundantVMStates.empty();
}

void RemoveRedundantVMStates::transferInstruction(
    Instruction *I, DenseSet<CallInst *> *vmStatesAvailable) {
  if (!canBeSafelyReplayed(I)) {
    vmStatesAvailable->clear();
  } else if (isJVMState(I)) {
    vmStatesAvailable->insert(cast<CallInst>(I));
  }
}

bool RemoveRedundantVMStates::canBeSafelyReplayed(const Instruction *I) {
  // Any store -> bad
  // Any volatile load -> bad
  // Any fence, cmpxcmp, atomicrmw -> bad
  // Any call -> bad
  // Any write to memory -> bad
  // (could be more aggressive on several)
  // terminators -> safe
  // math -> safe
  // unordered load -> safe
  if (const StoreInst *SI = dyn_cast<StoreInst>(I)) {
    // The anchor instruction is the only store instruction that can
    // be safely replayed.
    return isJVMStateAnchorInstruction(SI);
  }

  if (const LoadInst* LI = dyn_cast<LoadInst>(I)) {
    // Be conservative for the moment and say that any 'special' load can not
    // be replayed safely.
    if (LI->isAtomic() || LI->isVolatile() || !LI->isUnordered()) {
      return false;
    }
    // fallthrough
  }

  // Be conservative, anything with ordering or write semantics,
  // assume we can't replay
  if (isa<FenceInst>(I) || isa<AtomicCmpXchgInst>(I) ||
      isa<AtomicRMWInst>(I)) {
    return false;
  }

  if (isa<CallInst>(I) || isa<InvokeInst>(I)) {
    if (isJVMState(I)) {
      return true;
    }

    ImmutableCallSite CS(I);
    // Note: It is likely NOT safe to use read only here.  The
    // function could contain volatile load or fence with implied
    // ordering.  (Not sure about the exact semantics here, so be
    // conservative)
    //
    // if( CS.onlyReadsMemory() ) {
    //   return true;
    // }
    if (const Function *F = CS.getCalledFunction()) {
      // This routine is known (by the frontend) to be idempotent and
      // replayable.  This attribute could probably be generalized for other
      // purposes, but for now, we use one specific to this purpose
      if( F->getFnAttribute("vmstate-idempotent").getValueAsString().equals("true") ) {
        return true;
      }
    }
    return false;
  }

  return !I->mayWriteToMemory();
}

void RemoveRedundantVMStates::removeVMStateCalls(const DenseSet<CallInst *> &calls) {
  DenseSet<Function *> declarationsToRemove;
  for (DenseSet<CallInst *>::const_iterator I = calls.begin(), E = calls.end();
       I != E;
       ++I) {
    const CallInst *CI = *I;

    assert(isJVMState(CI) && "This routine should only be used to"
           " remove vmstates");
    Function *F = CI->getCalledFunction();
    assert(F && F->isDeclaration() && "must be a direct to a function with an empty body");
    declarationsToRemove.insert(F);
  }

  for (DenseSet<CallInst *>::const_iterator I = calls.begin(), E = calls.end();
       I != E;
       ++I) {
    CallInst *CI = *I;

    // Remove the use holding this call in place
    assert(CI->hasOneUse() && "must have exactly one use");
    StoreInst *use = cast<StoreInst>(*CI->use_begin());
    assert(isJVMStateAnchorInstruction(use) && "only possible use");
    use->eraseFromParent();
    assert(use->hasNUses(0) && "should be no uses left");

    // Remove the call itself
    CI->eraseFromParent();
  }

  // Remove the functions which are now dead - note that the use of a
  // set is required since calls can be duplicated by the optimizer
  for (DenseSet<Function*>::iterator I = declarationsToRemove.begin(),
                                     E = declarationsToRemove.end();
      I != E;
      ++I) {

    // This will have to change once we support inlining.  We'll
    // probably then have to RAUW this value.
    (*I)->eraseFromParent();
  }
}

void RemoveRedundantVMStates::getVMStatesDominatingBB(
    BasicBlock *BB, DominatorTree *DT, DenseSet<CallInst *> *dominatingVMStates) {
  for (DomTreeNode *currentNode = DT->getNode(BB); currentNode;
       currentNode = currentNode->getIDom()) {
    BasicBlock *dominatingBlock = currentNode->getBlock();
    for (BasicBlock::iterator I = dominatingBlock->begin(), E = dominatingBlock->end();
         I != E;
         ++I) {
      if (isJVMState(I)) {
        dominatingVMStates->insert(cast<CallInst>(I));
      }
    }
  }
}

char RemoveRedundantVMStates::ID = 0;

FunctionPass *llvm::createRemoveRedundantVMStatesPass() {
  return new RemoveRedundantVMStates();
}

INITIALIZE_PASS_BEGIN(RemoveRedundantVMStates,
                "remove-redundant-vm-states", "", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(RemoveRedundantVMStates,
                    "remove-redundant-vm-states", "", false, false)
