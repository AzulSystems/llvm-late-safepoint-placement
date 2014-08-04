/** Run a sanity check on the IR to ensure that Safepoints - if they've been
   inserted - were inserted correctly.  In particular, look for use of
   non-relocated values after a safepoint.  It's primary use is to check
   the correctness of safepoint insertion immediately after insertion, but
   it can also be used to verify that later transforms have not found a
   way to break safepoint semenatics.  

   It it's current form, this verify is checks a property which is
   sufficient, but not neccessary for correctness.  There are some cases
   where an unrelocated pointer can be used after the safepoint.  Consider
   this example:
   a = ...
   b = ...
   (a',b') = safepoint(a,b)
   c = cmp eq a b
   br c, ..., ....

   Because it is valid to reorder 'c' above the safepoint, this is legal.
   In practice, this is a somewhat uncommon transform, but CodeGenPrep
   does create idioms like this.  Today, the verifier would report a
   spuriour failure on this case.
 */
#define DEBUG_TYPE "safepoint-ir-verifier"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/IR/SafepointIRVerifier.h"

using namespace llvm;
using namespace std;

static cl::opt<bool> AllowNonEscapingUnrelocatedValues(
    "spp-verifier-allow-non-escaping-unrelocated-values", cl::init(false));

/// This option is used for writting test cases.  Instead of
/// crashing the program when verification fails, report a
/// message to the console (for FileCheck usage) and continue
/// execution as if nothing happened.
static cl::opt<bool> PrintOnly("safepoint-ir-verifier-print-only",
                               cl::init(false));

struct SafepointIRVerifier : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  DominatorTree DT;
  SafepointIRVerifier() : FunctionPass(ID) {
    initializeSafepointIRVerifierPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.setPreservesAll();
  }
};

namespace {
// Walk through the def-use chains and seek out any other Value which is
// simply a GEP or bitcast from the one we know got invalidated.  Since the
// object may have moved, all these are invalid as well.
// Note: We can't walk PHIs or Selects despite how tempting it might seem.
// Without reasoning about control flow, that might not have been the dynamic
// value which got invalidated.
void add_transative_closure(Value *gcptr, DenseSet<Value *> &invalid) {
  if (invalid.find(gcptr) != invalid.end()) {
    // base case - do not continue
    return;
  }

  // TODO-PERF: separate an inner function to allow use of batched insert

  // we dont want to add a null into the invalid
  if (isa<Constant>(gcptr)) {
    assert((isa<ConstantPointerNull>(gcptr) || isa<UndefValue>(gcptr)) &&
           "We should not record a constant as a gc pointer except for null");
  } else {
    invalid.insert(gcptr);
  }
  // First walk up the def chain if there's anything above us that should
  // have been invalidated as well.
  if (Instruction *inst = dyn_cast<Instruction>(gcptr)) {
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(inst)) {
      Value *Op = GEP->getOperand(0);
      add_transative_closure(Op, invalid);
    } else if (BitCastInst *cast = dyn_cast<BitCastInst>(inst)) {
      Value *Op = cast->getOperand(0);
      add_transative_closure(Op, invalid);
    }
  }

  // Second, walk through all of our uses looking for things which should
  // have been invalidated as well.
  for (Value::user_iterator I = gcptr->user_begin(), E = gcptr->user_end();
       I != E; I++) {
    if (isa<CastInst>(*I) || isa<GetElementPtrInst>(*I)) {
      add_transative_closure(*I, invalid);
    }
  }
}

void add_dominating_defs(Instruction *term, DenseSet<Value *> &invalid,
                         DominatorTree *DT) {
  // This function is basically a copy from
  // SafepointPlacementImpl::findLiveGCValuesAtInst
  // The only difference is that we record all possible gc pointers which
  // dominate the safepoint and dont do any liveness check.
  assert(!isa<PHINode>(term) &&
         "term shoud be a safepoint and can not be a phi node");
  Function *F = term->getParent()->getParent();
  for (Function::arg_iterator argitr = F->arg_begin(), argend = F->arg_end();
       argitr != argend; argitr++) {
    Argument &arg = *argitr;
    if (isGCPointerType(arg.getType())) {
      add_transative_closure(&arg, invalid);
    }
  }

  BasicBlock *pred = term->getParent();
  for (DomTreeNode *currentNode = DT->getNode(pred); currentNode;
       currentNode = currentNode->getIDom()) {
    BasicBlock *BBI = currentNode->getBlock();
    assert(isPotentiallyReachable(BBI, pred) &&
           "dominated block must be reachable");
    for (BasicBlock::iterator itr = BBI->begin(), end = BBI->end(); itr != end;
         itr++) {
      Instruction &inst = *itr;

      if (pred == BBI && (&inst) == term) {
        break;
      }

      if (!isGCPointerType(inst.getType())) {
        continue;
      }

      if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(&inst)) {
        if (II->getIntrinsicID() == Intrinsic::statepoint) {
          continue;
        }
      }
      add_transative_closure(&inst, invalid);
    }
  }
}

struct bb_state {
  /// The set of values which are known to be invalid on entry to
  /// the basic block.  WARNING: PHIs may validly contain uses of
  /// values in this set.  This can be used to avoid traversing the
  /// rest of the basic block, but you must still scan the PHI nodes!
  DenseSet<Value *> InvalidOnEntry;

  /// The set of values known to be invalid on exit from the basic
  /// block.
  DenseSet<Value *> InvalidOnExit;
};
}

static void ComputeInvalidSetAtStatepoint(CallSite &CS,
                                          DenseSet<Value *> &invalid,
                                          DominatorTree &DT) {
  Instruction *inst = CS.getInstruction();
  Statepoint statepoint(CS);

  for (ImmutableCallSite::arg_iterator i = statepoint.gc_args_begin(),
                                       e = statepoint.gc_args_end();
       i != e; i++) {
    Value *op = *i;

    assert(isa<Argument>(op) || isa<Constant>(op) || isa<Instruction>(op));

    // Add this Value and any other pointers we can find to the same
    // base object -- all are invalid to use after the safepoint
    add_transative_closure(op, invalid);
  }

  add_dominating_defs(inst, invalid, &DT);

  // TODO: we should expand the invalidation set to include the
  // transative closure of bitcasts and geps (both uses and ops) from
  // the invalidated Values.  Those are invalid too, but might not be
  // explicitly listed due to liveness.
}

static bool RelocationPHIEscapes(PHINode *node) {
  if (!AllowNonEscapingUnrelocatedValues) {
    return true;
  }
  // initial allocation of 200 picked more or less at random
  SmallPtrSet<PHINode *, 200> explored;
  SmallVector<PHINode *, 200> worklist;
  worklist.push_back(node);

  while (!worklist.empty()) {
    PHINode *node = worklist.back();
    worklist.pop_back();
    explored.insert(node);
    if (!node->getMetadata("is_relocation_phi"))
      return true;

    for (Value::user_iterator I = node->user_begin(), E = node->user_end();
         I != E; ++I) {
      if (PHINode *node = dyn_cast<PHINode>(*I)) {
        if (explored.count(node) == 0)
          worklist.push_back(node);
      } else {
        return true;
      }
    }
  }
  return false;
}

/// A helper struct to manager insertion and removal into
/// a worklist.  Items are processed in LIFO order except
/// that items are only inserted once and not duplicated.
/// This 'no multiple visits' property has a major impact
/// on large IR graphs.  (80% reduction on motivating example)
/// The order is unimportant for correctness, but is
/// general considered good for data flow convergence.
/// (no data has been gathered for that last point)
struct Worklist {
  // initial allocation of 200 picked more or less at random
  // we want it large enough for most functions, but small
  // enough to not waste tons of stack space.  Given we
  // should be shallow and near a leaf at this point, we
  // could afford to be even more generous.
  SmallVector<BasicBlock *, 200> Worklist;
  DenseSet<BasicBlock *> OnList;

  void push_back(BasicBlock *BB) {
    if (OnList.count(BB)) {
      // already in the worklist
      return;
    }
    Worklist.push_back(BB);
    OnList.insert(BB);
  }
  BasicBlock *next() {
    BasicBlock *BB = Worklist.back();
    Worklist.pop_back();

    OnList.erase(BB);
    return BB;
  }

  bool empty() const {
    return Worklist.empty();
  }
};

bool SafepointIRVerifier::runOnFunction(Function &F) {
  DT.recalculate(const_cast<Function &>(F));
  /* TODO: Additional invariants to check
     - There can be exactly one 'original' value in each relocation phi.
     - Each basic block can contain at most one relocation phi for each
     original value.
   */

  // All states start empty
  DenseMap<BasicBlock *, bb_state> state;
  Worklist worklist;

  DenseMap<Instruction *, DenseSet<Value *>> StatepointInvalidations;

  // Start with all of the blocks containing statepoints, iterate from there to
  // establish and check the invalid sets
  for (inst_iterator itr = inst_begin(F), end = inst_end(F); itr != end;
       itr++) {
    Instruction *inst = &*itr;
    if (isa<InvokeInst>(inst) || isa<CallInst>(inst)) {
      CallSite CS(inst);
      Function *F = CS.getCalledFunction();
      if (F && F->getIntrinsicID() == Intrinsic::statepoint) {
        worklist.push_back(inst->getParent());

        // The invalidation set for a given instruction doesn't change
        // during iteration.  Since computing the invalid set for a statepoit
        // is rather expensive, recompute it once and cache the result.
        ComputeInvalidSetAtStatepoint(CS, StatepointInvalidations[inst], DT);
      }
    }
  }

  // iterate until the invalid states stablize, checking on every iteration.
  // The check could be pulled into a single post pass, but why bother?
  while (!worklist.empty()) {
    BasicBlock *current = worklist.next();

    DenseSet<Value *> nowvalid;
    // First, handle all the PHINodes in a path sensative manner
    for (BasicBlock::iterator itr = current->begin(),
                              end = current->getFirstNonPHI();
         itr != end; itr++) {
      Instruction *inst = &*itr;
      PHINode *phi = cast<PHINode>(inst);
      // The 'use' check for a phi needs to be path sensative.  Remember, a
      // phi use is valid if the use if valid in the source block, not the
      // current block!
      for (size_t i = 0; i < phi->getNumIncomingValues(); i++) {
        Value *InVal = phi->getIncomingValue(i);
        BasicBlock *inBB = phi->getIncomingBlock(i);
        if (state[inBB].InvalidOnExit.find(InVal) !=
            state[inBB].InvalidOnExit.end()) {
          if (RelocationPHIEscapes(phi)) {
            errs() << "Illegal use of unrelocated value in phi edge-reachable "
                      "from safepoint found!\n";
            errs() << "Def: ";
            InVal->dump();
            errs() << "Use: ";
            inst->dump();
            if (!PrintOnly)
              assert(state[inBB].InvalidOnExit.find(InVal) ==
                         state[inBB].InvalidOnExit.end() &&
                     "use of invalid unrelocated value after safepoint!");
          }
        }
      }
      nowvalid.insert(phi);
    }

    // Anything invalid an _any_ of our input blocks is invalid in this one
    DenseSet<Value *> invalid;
    for (pred_iterator PI = pred_begin(current), E = pred_end(current);
         PI != E; ++PI) {
      BasicBlock *Pred = *PI;
      bb_state exit = state[Pred];
      invalid.insert(exit.InvalidOnExit.begin(), exit.InvalidOnExit.end());
    }

    // We don't need to rescan the block if the state on entry is the same.
    // Note that we DO have to perform the path sensative phi check above
    // since a value could be newly invalidated along some particular edge
    if (!invalid.empty() &&
        set_equals(state[current].InvalidOnEntry, invalid)) {
      // The empty check is to ensure we process the bb at least once.
      // Since an empty invalid set doesn't contribute to the invalid
      // set of it's successor, we shouldn't renter a bb with the set empty
      continue;
    }
    state[current].InvalidOnEntry = invalid;

    // If we encounter a def via a backedge, remove it from the set of
    // invalid uses - in this case, all phi defs are valid, no matter what cam
    // in through the merge set
    set_subtract(invalid, nowvalid);

    // Then scan through all the rest of the instructions, checking for invalid
    // uses and
    for (BasicBlock::iterator itr = current->getFirstNonPHI(),
                              end = current->end();
         itr != end; itr++) {
      Instruction *inst = &*itr;

      // Check all the uses
      for (size_t i = 0; i < inst->getNumOperands(); i++) {
        Value *op = inst->getOperand(i);
        if (invalid.find(op) != invalid.end()) {
          errs()
              << "Illegal use of unrelocated value after safepoint found!\n";
          errs() << "Def: ";
          op->dump();
          errs() << "Use: ";
          inst->dump();
          if (!PrintOnly)
            assert(invalid.find(op) == invalid.end() &&
                   "use of invalid unrelocated value after safepoint!");
        }
      }

      // if this is a statepoint, expand the invalid set.  Anything relocated
      // by this safepoint is now invalid.
      if (isa<InvokeInst>(inst) || isa<CallInst>(inst)) {
        CallSite CS(inst);
        Function *F = CS.getCalledFunction();
        if (F && F->getIntrinsicID() == Intrinsic::statepoint) {

          // see the note above about stablity and precomputation
          assert(
              StatepointInvalidations.count(inst) &&
              "must have precomputed an invalidation set for each statepoint");

          set_union(invalid, StatepointInvalidations[inst]);
        }
      }
      // If we encounter a def via a backedge, remove it from the set of
      // invalid uses
      invalid.erase(inst);
    }

    // If our output has changed, add any successor blocks to the worklist
    if (!set_equals(state[current].InvalidOnExit, invalid)) {
#if 0
      //TODO: Add some logging
      if( TraceIVMSP ) {
        errs() << "update: " << Succ << " ";
        state[Succ].dump();
        errs() << " -> ";
        entry.dump();
        errs() << "\n";
      }
#endif
      state[current].InvalidOnExit = invalid;
      if (!invalid.empty()) {
        // We are guaranteed to visit each statepoint block once since
        // we manually added them on the worklist.  These are the only
        // blocks which can map empty => nonempty on anything other
        // than the first iteraion.
        for (succ_iterator PI = succ_begin(current), E = succ_end(current);
             PI != E; ++PI) {
          BasicBlock *Succ = *PI;
          // TODO-PERF: could prune if invalid subset of successor incoming
          // and we walked the phis eagerly here to handle path sensativity
          worklist.push_back(Succ);
        }
      }
    }
  } // while( !worklist.empty() )

  // No modification, ever
  return false;
}

void llvm::verifySafepointIR(Function &F) {
  SafepointIRVerifier pass;
  pass.runOnFunction(F);
}

char SafepointIRVerifier::ID = 0;

FunctionPass *llvm::createSafepointIRVerifierPass() {
  return new SafepointIRVerifier();
}

INITIALIZE_PASS_BEGIN(SafepointIRVerifier, "verify-safepoint-ir", "", false,
                      true)
INITIALIZE_PASS_END(SafepointIRVerifier, "verify-safepoint-ir", "", false,
                    true)
