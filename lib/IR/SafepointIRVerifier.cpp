/** Run a sanity check on the IR to ensure that Safepoints - if they've been
   inserted - were inserted correctly.  In particular, look for use of
   non-relocated values after a safepoint.
 */
#define DEBUG_TYPE "safepoint-erasure"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/CFG.h"
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
#include "llvm/Support/CallSite.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/IR/SafepointIRVerifier.h"

using namespace llvm;
using namespace std;

cl::opt<bool> AllowNonEscapingUnrelocatedValues(
    "spp-verifier-allow-non-escaping-unrelocated-values",
    cl::init(false));

struct SafepointIRVerifier : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  DominatorTree DT;
  SafepointIRVerifier() : FunctionPass(ID) {
    initializeSafepointIRVerifierPass(*PassRegistry::getPassRegistry());
  }

  virtual bool runOnFunction(Function &F);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
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
  void add_transative_closure(Value* gcptr, std::set<Value*>& invalid) {
    if( invalid.find(gcptr) != invalid.end() ) {
      // base case - do not continue
      return;
    }

    // we dont want to add a null into the invalid
    if (isa<Constant>(gcptr)) {
      assert(isa<ConstantPointerNull>(gcptr) && "We should not record a constant as a gc pointer except for null");
    } else {
      invalid.insert(gcptr);
    }
    // First walk up the def chain if there's anything above us that should
    // have been invalidated as well.
    if(Instruction* inst =  dyn_cast<Instruction>(gcptr) ) {
      if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(inst)) {
        Value *Op = GEP->getOperand(0);
        add_transative_closure(Op, invalid);
      } else if( BitCastInst* cast = dyn_cast<BitCastInst>(inst) ) {
        Value *Op = cast->getOperand(0);
        add_transative_closure(Op, invalid);
      }
    }

    // Second, walk through all of our uses looking for things which should
    // have been invalidated as well.
    for (Value::use_iterator I = gcptr->use_begin(), E = gcptr->use_end();
         I != E; I++) {
      if( isa<CastInst>(*I) || isa<GetElementPtrInst>(*I) ) {
        add_transative_closure(*I, invalid);
      }
    }
  }
  
  void add_dominating_defs(Instruction* term, std::set<Value*>& invalid, DominatorTree* DT) {
    // This function is basically a copy from SafepointPlacementImpl::findLiveGCValuesAtInst
    // The only difference is that we record all possible gc pointers which
    // dominate the safepoint and dont do any liveness check.
    assert(!isa<PHINode>(term) && "term shoud be a safepoint and can not be a phi node");
    Function* F = term->getParent()->getParent();
    for(Function::arg_iterator argitr = F->arg_begin(), argend = F->arg_end();
        argitr != argend; argitr++) {
      Argument& arg = *argitr;
      if( isGCPointerType(arg.getType()) ) {
        add_transative_closure(&arg, invalid);
      }
    }

    BasicBlock *pred = term->getParent();
    for (DomTreeNode *currentNode = DT->getNode(pred); currentNode; currentNode = currentNode->getIDom()) {
      BasicBlock *BBI = currentNode->getBlock();
      assert( isPotentiallyReachable(BBI, pred) && "dominated block must be reachable");
      for(BasicBlock::iterator itr = BBI->begin(), end = BBI->end();
          itr != end; itr++) {
        Instruction& inst = *itr;

        if( pred == BBI && (&inst) == term ) {
          break;
        }

        if( !isGCPointerType(inst.getType()) ) {
          continue;
        }

        if( IntrinsicInst *II = dyn_cast<IntrinsicInst>(&inst) ) {
          if( II->getIntrinsicID() == Intrinsic::statepoint ) {
            continue;
          }
        }
        add_transative_closure(&inst, invalid);
      }
    }
  }

  struct bb_exit_state {
    set<Value*> _invalid;
  };
}

static bool RelocationPHIEscapes(PHINode *node) {
  if (!AllowNonEscapingUnrelocatedValues) {
    return true;
  }
  set<PHINode *> explored;
  vector<PHINode *> worklist;
  worklist.push_back(node);

  while (!worklist.empty()) {
    PHINode *node = worklist.back();
    worklist.pop_back();
    explored.insert(node);
    if (!node->getMetadata("is_relocation_phi")) return true;

    for (Value::use_iterator I = node->use_begin(), E = node->use_end(); I != E; ++I) {
      if (PHINode *node = dyn_cast<PHINode>(*I)) {
        if (explored.count(node) == 0) worklist.push_back(node);
      } else {
        return true;
      }
    }
  }
  return false;
}

bool SafepointIRVerifier::runOnFunction(Function &F) {
  DT.recalculate(const_cast<Function &>(F));
  /* TODO: Additional invariants to check
     - There can be exactly one 'original' value in each relocation phi.
     - Each basic block can contain at most one relocation phi for each
     original value.
   */

  

  // All states start empty
  std::map<BasicBlock*, bb_exit_state> state;

  std::vector<BasicBlock*> worklist;

  // Start with all of the blocks containing statepoints, iterate from there to
  // establish and check the invalid sets
  for( inst_iterator itr = inst_begin(F), end = inst_end(F);
       itr != end; itr++) {
    Instruction* inst = &*itr;
    if( isa<InvokeInst>(inst) || isa<CallInst>(inst) ) {
      CallSite CS(inst);
      Function* F = CS.getCalledFunction();
      if( F && F->getIntrinsicID() == Intrinsic::statepoint ) {
        worklist.push_back( inst->getParent() );
      }
    }
  }

  // iterate until the invalid states stablize, checking on every iteration.
  // The check could be pulled into a single post pass, but why bother?
  while( !worklist.empty() ) {
    BasicBlock* current = worklist.back();
    worklist.pop_back();

    set<Value*> nowvalid;
    // First, handle all the PHINodes in a path sensative manner
    for(BasicBlock::iterator itr = current->begin(), end = current->getFirstNonPHI();
        itr != end; itr++) {
      Instruction* inst = &*itr;
      PHINode* phi = cast<PHINode>(inst);
      // The 'use' check for a phi needs to be path sensative.  Remember, a
      // phi use is valid if the use if valid in the source block, not the
      // current block!
      for(size_t i = 0; i < phi->getNumIncomingValues(); i++) {
        Value *InVal = phi->getIncomingValue(i);
        BasicBlock* inBB = phi->getIncomingBlock(i);
        if( state[inBB]._invalid.find(InVal) != state[inBB]._invalid.end() ) {
          errs() << "Illegal use of unrelocated value in phi edge-reachable from safepoint found!\n";
          errs() << "Def: ";
          InVal->dump();
          errs() << "Use: ";
          inst->dump();
          if (RelocationPHIEscapes(phi)) {
            assert( state[inBB]._invalid.find(InVal) == state[inBB]._invalid.end() &&
                    "use of invalid unrelocated value after safepoint!");
          }
        }
      }
      nowvalid.insert(phi);
    }

    // Anything invalid an _any_ of our input blocks is invalid in this one
    std::set<Value*> invalid;
    for (pred_iterator PI = pred_begin(current), E = pred_end(current); PI != E; ++PI) {
      BasicBlock *Pred = *PI;
      bb_exit_state exit = state[Pred];
      invalid.insert(exit._invalid.begin(), exit._invalid.end());
    }

    // If we encounter a def via a backedge, remove it from the set of
    // invalid uses - in this case, all phi defs are valid, no matter what cam
    // in through the merge set
    for(set<Value*>::iterator itr = nowvalid.begin(), end = nowvalid.end();
        itr != end; itr++) {
      set<Value*>::iterator invalid_itr = invalid.find(*itr);
      if( invalid_itr != invalid.end() ) {
        invalid.erase(invalid_itr);
      }
    }
    
    // Then scan through all the rest of the instructions, checking for invalid
    // uses and 
    for(BasicBlock::iterator itr = current->getFirstNonPHI(), end = current->end();
        itr != end; itr++) {
      Instruction* inst = &*itr;

      // Check all the uses
      size_t StartIdx = 0;
      if( isa<InvokeInst>(inst) || isa<CallInst>(inst) ) {
        CallSite CS(inst);
        Function* F = CS.getCalledFunction();
        if( F && F->getIntrinsicID() == Intrinsic::statepoint ) {
          // There's a known bug - which we don't want to trip over at the
          // moment - where uses in VMStates don't get updated properly.  This
          // only matters for deopt, so WONTFIX (at the moment).
          const int num_call_args = cast<ConstantInt>(CS.getArgument(1))->getZExtValue();
          const int num_stacks = cast<ConstantInt>(CS.getArgument(4))->getZExtValue();
          const int num_locals = cast<ConstantInt>(CS.getArgument(5))->getZExtValue();
          const int num_mon = cast<ConstantInt>(CS.getArgument(6))->getZExtValue();

          const int gc_begin = 7 + num_call_args + num_stacks + num_locals + num_mon;
          assert( gc_begin <= std::distance(CS.arg_begin(), CS.arg_end()) );
          StartIdx = gc_begin;
        }
      }
      for(size_t i = StartIdx; i < inst->getNumOperands(); i++) {
        Value* op = inst-> getOperand(i);
        if( invalid.find(op) != invalid.end() ) {
          errs() << "Illegal use of unrelocated value after safepoint found!\n";
          errs() << "Def: ";
          op->dump();
          errs() << "Use: ";
          inst->dump();
          assert( invalid.find(op) == invalid.end() &&
                  "use of invalid unrelocated value after safepoint!");
        }
      }

      // if this is a statepoint, expand the invalid set.  Anything relocated
      // by this safepoint is now invalid.
      if( isa<InvokeInst>(inst) || isa<CallInst>(inst) ) {
        CallSite CS(inst);
        Function* F = CS.getCalledFunction();
        // During safepoint insertion, we use ignore-clobber to mark the
        // current safepoint before relocations are applied.  This is basically
        // so we can check newly inserted values such as base pointers which
        // respect to the recursive assumptions used by relocation.
        if( F && F->getIntrinsicID() == Intrinsic::statepoint &&
            !inst->getMetadata("ignore_clobber") ) {
          const int num_call_args = cast<ConstantInt>(CS.getArgument(1))->getZExtValue();
          const int num_stacks = cast<ConstantInt>(CS.getArgument(4))->getZExtValue();
          const int num_locals = cast<ConstantInt>(CS.getArgument(5))->getZExtValue();
          const int num_mon = cast<ConstantInt>(CS.getArgument(6))->getZExtValue();

          const int gc_begin = 7 + num_call_args + num_stacks + num_locals + num_mon;
          assert( gc_begin <= std::distance(CS.arg_begin(), CS.arg_end()) );

          for(int i = gc_begin; i < std::distance(CS.arg_begin(), CS.arg_end()); i++) {
            Value* op = CS.getArgument(i);

            assert( isa<Argument>(op) || isa<Constant>(op) || isa<Instruction>(op) );

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
      }
      // If we encounter a def via a backedge, remove it from the set of
      // invalid uses
      if( invalid.find(inst) != invalid.end() ) {
        invalid.erase(inst);
      }
    }

    // If our output has changed, add any successor blocks to the worklist
    if( state[current]._invalid != invalid ) {
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
      state[current]._invalid = invalid;
      for (succ_iterator PI = succ_begin(current), E = succ_end(current); PI != E; ++PI) {
        BasicBlock *Succ = *PI;
        worklist.push_back(Succ);
      }
    }
  } // while( !worklist.empty() )

  // No modification, ever
  return false;
}

void llvm::verifySafepointIR(Function& F) {
  SafepointIRVerifier pass;
  pass.runOnFunction(F);
}

char SafepointIRVerifier::ID = 0;

FunctionPass *llvm::createSafepointIRVerifierPass() {
  return new SafepointIRVerifier();
}

INITIALIZE_PASS_BEGIN(SafepointIRVerifier,
                "verify-safepoint-ir", "", false, true)
INITIALIZE_PASS_END(SafepointIRVerifier,
                    "verify-safepoint-ir", "", false, true)

