#define DEBUG_TYPE "merge-vmstates"

#include "llvm/ADT/DenseSet.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/JVMState.h"
#include "llvm/IR/Module.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"

#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <vector>

using namespace llvm;

extern cl::opt<bool> AllFunctions;

// Before we start optimization (i.e. after parsing and removal of
// redundant VM states), we have the nice invariant that every call or
// location that may eventually require a safepoint has a dominating
// VM state (a jvmstate_ call whose result is written to an anchor
// variable) that is correct to use.  However, this may not be true
// after optimization -- consider the IR fragemnt below (the writes to
// the anchor variable has been elided for consistency):
//
//  jvmstate_0(...)
//  jvmstate_1(...)
//  call void @parse_point() ;; jvmstate_1 is the dominating vm state
//
// This may get optimized to
//
//  jvmstate_0(...)
//  br i1 undef, label %a, label %b
//
// a:
//  jvmstate_1(...)
//  br label %merge
//
// b:
//  jvmstate_1(...)
//  br label %merge
//
// merge:
//  call void @parse_point() ;; jvmstate_0 is the dominating vm state now
//
// Naively picking the dominating VM state for @parse_point will
// result in picking jvmstate_0, which is incorrect.
//
// This pass transforms IR like the above to
//
//  jvmstate_0(...)
//  br i1 undef, label %a, label %b
//
// a:
//  jvmstate_1(...)
//  br label %merge
//
// b:
//  jvmstate_1(...)
//  br label %merge
//
// merge:
//  jvmstate_1({ operand-wise phi of each of the values in %a and %b's
//               jvmstate_1})
//  call void @parse_point() ;; jvmstate_0 is the dominating vm state now
//

namespace {

struct MergeNonDominatingVMStates : public FunctionPass {
  static char ID;
  std::vector<Instruction *> locations;
  bool shouldComputeLocations;

  // The pass be parameterized with the set of Instructions *'s that
  // are supposed to have a dominating jvmstate_.
  explicit MergeNonDominatingVMStates(const std::vector<Instruction *> &locs)
      : FunctionPass(ID), locations(locs), shouldComputeLocations(false) {
    initializeMergeNonDominatingVMStatesPass(*PassRegistry::getPassRegistry());

    assert(!AllFunctions && "we don't handle AllFunctions!");

#ifndef NDEBUG
    DenseSet<Instruction *> uniqued;
    uniqued.insert(locs.begin(), locs.end());
    assert(uniqued.size() == locs.size() && "no duplicates please!");
#endif
  }

  // If it isn't parameterized with the list of locations that need
  // dominating jvmstate_s, all the calls in the Function it is being
  // run on is used to construct that list.
  MergeNonDominatingVMStates() : FunctionPass(ID), shouldComputeLocations(true) {
    initializeMergeNonDominatingVMStatesPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    if (!shouldGetSafepoints(F)) { return false; }
    if (shouldComputeLocations) { computeLocations(F); }
    DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    return mergeVMStates(F, DT);
  }

  bool mergeVMStates(Function &, DominatorTree &);
  void phiOfVMStatesToVMStateOfPhis(PHINode *, StoreInst *, DominatorTree &DT);

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<DominatorTreeWrapperPass>();
  }

  // shouldGetSafepoints(F) is true if F needs safepoints.
  bool shouldGetSafepoints(Function &F);
  bool isFunctionAttrTrue(Function &F, const char *);

  void computeLocations(Function &);
  bool isUseInFunction(Instruction *, Function &);
};

} // end anonymous namespace


bool MergeNonDominatingVMStates::isUseInFunction(Instruction *Instr, Function &F) {
  return Instr->getParent()->getParent() == &F;
}

bool MergeNonDominatingVMStates::shouldGetSafepoints(Function &F) {
  return isFunctionAttrTrue(F, "gc-add-backedge-safepoints") ||
      isFunctionAttrTrue(F, "gc-add-call-safepoints") ||
      isFunctionAttrTrue(F, "gc-add-entry_safepoints");
}

bool MergeNonDominatingVMStates::isFunctionAttrTrue(Function &F,
                                                    const char *attr) {
  return F.getFnAttribute(attr).getValueAsString().equals("true");
}

// Main entry point for the algorithm
bool MergeNonDominatingVMStates::mergeVMStates(Function &F, DominatorTree &DT) {
  const char *anchorName = "llvm.jvmstate_anchor";
  Module &M = *F.getParent();

  GlobalVariable *anchorVariable = cast_or_null<GlobalVariable>(
      M.getNamedValue(anchorName));
  if (anchorVariable == nullptr) {
    assert(locations.empty() &&
           "we better not have any real work to do if we can't locate the anchor!");
    return false;
  }

#ifndef NDEBUG
  for (User *U : anchorVariable->users()) {
    assert(isJVMState(cast<StoreInst>(U)->getValueOperand()) &&
           "sanity: all stores to llvm.jvmstate_anchor should be "
           "jvmstate_ calls");
  }
#endif

  // No jvmstate_s we need to worry about, return early.  We can still
  // have VM states in the IR, as call VM states that don't manifest
  // as stores to the jvmstate_anchor
  if (anchorVariable->use_empty()) { return false; }

  // The core algorithm works by abusing the mem2reg pass.  Every
  // jvmstate_ we care about already is (volatile) stored to a global
  // variable to hold it in place.
  //
  //  1. We first replace that global variable (the anchor) with a
  //     local alloca, and insert a volatile store of a {load from
  //     that alloca} before every instruction that needs to see a
  //     dominating VM state.
  //
  //  2. Then we run the mem2reg pass which promotes all these loads
  //     and stores to phis and SSA values, and every new volatile
  //     store to the anchor (the ones we just inserted in 1.) now
  //     either stores a call to jvmstate_ (this will be true if we
  //     already had a dominating VM state) or a phi with every
  //     incoming value to the phi a jvmstate_ for the same bci (if
  //     this isn't true something unexpected happened, and we crash).
  //
  //  3. We then convert the stores that store the result of a phi of
  //     jvmstate_s into jvmstate_s of phis.  The algorithm that does
  //     this is documented in phiOfVMStatesToVMStateOfPhis.  After
  //     this we're done.
  //


  Type *anchorType = cast<PointerType>(anchorVariable->getType())->getElementType();

  // This is the alloca mentioned in (1).
  AllocaInst *allocaAnchor =
      new AllocaInst(anchorType, "alloca_anchor", F.getEntryBlock().getFirstNonPHI());

  std::vector<StoreInst *> anchorUsers;

  for (User *U : anchorVariable->users()) {
    if (isUseInFunction(cast<Instruction>(U), F))
      anchorUsers.push_back(cast<StoreInst>(U));
  }

  for (StoreInst *SI: anchorUsers) {
    SI->setVolatile(false);
    SI->replaceUsesOfWith(anchorVariable, allocaAnchor);
  }

  for (Instruction *I : locations) {
    // Add the stores mentioned in (1)
    LoadInst *currentVMState = new LoadInst(allocaAnchor, "current_vm_state", I);
    new StoreInst(currentVMState, anchorVariable, true /* isVolatile */, I);
  }

  assert(isAllocaPromotable(allocaAnchor) && "true by assumption!");
  PromoteMemToReg(allocaAnchor /* implicit conversion to ArrayRef */, DT);

  {
    // Go through the newly inserted stores to the anchor variable,
    // and transform phis of vmstates to vmstates of phis as required.
    // We first "stage" the StoreInst's in a std::vector because
    // phiOfVMStatesToVMStateOfPhis modifies use-def chains (it
    // changes the IR), and we don't want to be iterating over the
    // users list as we do that.

    std::vector<StoreInst *> worklist;

    for (User *U : anchorVariable->users()) {
      if (isUseInFunction(cast<Instruction>(U), F) && isa<PHINode>(cast<StoreInst>(U)->getValueOperand())) {
        worklist.push_back(cast<StoreInst>(U));
      }
    }

    for (StoreInst *SI : worklist) {
      phiOfVMStatesToVMStateOfPhis(cast<PHINode>(SI->getValueOperand()), SI, DT);
    }
  }

  return true;
}

void MergeNonDominatingVMStates::computeLocations(Function &F) {
  // In case the pass is created using the default constructor
  // (e.g. from the opt command line), create a list of interesting
  // locations that need to see a dominating VM state.
  for (auto I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    // We're intereted in calls that aren't jvmstate_ calls.
    if (isa<CallInst>(&*I) && !isJVMState(&*I)) { locations.push_back(&*I); }
  }
}

void MergeNonDominatingVMStates::phiOfVMStatesToVMStateOfPhis(
    PHINode *phi, StoreInst *anchorStore, DominatorTree &DT) {
  SmallVector<CallInst *, 4> vmStateCalls;

  // This takes a phi of jvmstates to a jvmstate of phis.

  {
    // We first do a search on the phi we're about to transform to get
    // the list of actual defining values.

    DenseSet<PHINode *> visited, workset;
    workset.insert(phi);

    while (!workset.empty()) {
      PHINode *currentPHI = *workset.begin();
      workset.erase(workset.begin());

      if (visited.count(currentPHI)) { continue; }
      visited.insert(currentPHI);

      for (Value *V : currentPHI->operands()) {
        if (PHINode *inputPHI = dyn_cast<PHINode>(V)) {
          workset.insert(inputPHI);
        } else {
          assert(isJVMState(V) && "real incoming values better be jvmstates");
          vmStateCalls.push_back(cast<CallInst>(V));
        }
      }
    }
  }

  assert(!vmStateCalls.empty() && "Must have at least one incoming VM state!");

  // firstCI (or firstCI) is not more significant than the other VM
  // states we have; we extract it out just to have convenient access
  // to the bci, expression stack size etc.
  CallInst *firstCI = vmStateCalls.front();
  JVMState firstJVMS(firstCI);
  assert(firstCI->getCalledFunction() != nullptr && "invalid VM state!");

  // The main algorithm proceeds by abusing the mem2reg pass.
  //
  //  1. We create a new alloca for every element in the VM state, and
  //     emit a store to that alloca before all the jvmstate_ calls.
  //
  //  2. We emit read from all the allocas to get the "current" value
  //     of the elements constituting the abstract state, and create a
  //     new jvmstate_ call right where we need the dominating
  //     jvmstate.
  //
  //  3. Running the mem2reg pass does the rest.
  //
  // Note that we can only do this because all the jvmstate_ calls
  // have the same "shape", i.e. the types and the number of their
  // arguments match up exactly.

  // The set of allocas mentioned in (1).
  std::vector<AllocaInst *> vmStateAllocaArgs;

  Instruction *allocaInsertPt = phi->getParent()->getParent()->getEntryBlock().getFirstNonPHI();
  for (auto I = firstCI->op_begin() + JVMState::headerEndOffset(),
            E = firstCI->op_begin() + firstCI->getNumArgOperands();
       I != E;
       ++I) {
    vmStateAllocaArgs.push_back(new AllocaInst(I->get()->getType(), "", allocaInsertPt));
  }

  for (CallInst *CI : vmStateCalls) {
    // For each jvmstate_ we have  ...
    assert(CI->getCalledFunction() == firstCI->getCalledFunction() && "mismatching VM states!");

    JVMState currentJVMS(CI);
    //    ... do some sanity checking
    assert(currentJVMS.bci() == firstJVMS.bci() && "mismatching VM states!");
    assert(currentJVMS.numStackElements() == firstJVMS.numStackElements() &&
           currentJVMS.numLocals() == firstJVMS.numLocals() &&
           currentJVMS.numMonitors() == firstJVMS.numMonitors() &&
           "mismatching VM states!");

    for (unsigned i = 0; i < vmStateAllocaArgs.size(); ++i) {
      //    ... and emit an argumentwise store into the allocas we
      //    created.
      new StoreInst(CI->getArgOperand(JVMState::headerEndOffset() + i),
                    vmStateAllocaArgs[i], CI);
    }
  }

  // The loads from the allocas mentioned in (2).
  std::vector<Value *> loadedVMStateArgs(
      firstCI->op_begin(), firstCI->op_begin() + JVMState::headerEndOffset());

  Instruction *loadInsertPt = phi->getParent()->getFirstNonPHI();
  for (AllocaInst *AllocI : vmStateAllocaArgs) {
    loadedVMStateArgs.push_back(new LoadInst(AllocI, "", loadInsertPt));
  }

  // The jvmstate we are interested in ...
  CallInst *newVMState = CallInst::Create(
      firstCI->getCalledFunction(), loadedVMStateArgs, "", anchorStore);
  new StoreInst(newVMState, anchorStore->getPointerOperand(),
                true /* isVolatile */, anchorStore);
  // ... which we store into the VM state anchor, and then we delete the original store.
  anchorStore->eraseFromParent();

#ifndef NDEBUG
  for (auto I = vmStateAllocaArgs.begin(), E = vmStateAllocaArgs.end(); I != E; ++I) {
    assert(isAllocaPromotable(*I) && "should be true by construction!");
  }
#endif

  PromoteMemToReg(vmStateAllocaArgs, DT);
}

FunctionPass *llvm::createMergeNonDominatingVMStatesPass() {
  return new MergeNonDominatingVMStates();
}

FunctionPass *llvm::createMergeNonDominatingVMStatesPass(
    const std::vector<Instruction *> &locations) {
  return new MergeNonDominatingVMStates(locations);
}

char MergeNonDominatingVMStates::ID = 0;

INITIALIZE_PASS_BEGIN(MergeNonDominatingVMStates,
                      "merge-non-dominating-vmstates",
                      "Merge non-dominting VM states", false, false)
INITIALIZE_PASS_END(MergeNonDominatingVMStates,
                    "merge-non-dominating-vmstates",
                    "Merge non-dominting VM states", false, false)
