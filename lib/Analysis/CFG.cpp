//===-- CFG.cpp - BasicBlock analysis --------------------------------------==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This family of functions performs analyses on basic blocks, and instructions
// contained within basic blocks.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CFG.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/Support/CallSite.h"

using namespace llvm;

/// FindFunctionBackedges - Analyze the specified function to find all of the
/// loop backedges in the function and return them.  This is a relatively cheap
/// (compared to computing dominators and loop info) analysis.
///
/// The output is added to Result, as pairs of <from,to> edge info.
void llvm::FindFunctionBackedges(const Function &F,
     SmallVectorImpl<std::pair<const BasicBlock*,const BasicBlock*> > &Result) {
  const BasicBlock *BB = &F.getEntryBlock();
  if (succ_begin(BB) == succ_end(BB))
    return;

  SmallPtrSet<const BasicBlock*, 8> Visited;
  SmallVector<std::pair<const BasicBlock*, succ_const_iterator>, 8> VisitStack;
  SmallPtrSet<const BasicBlock*, 8> InStack;

  Visited.insert(BB);
  VisitStack.push_back(std::make_pair(BB, succ_begin(BB)));
  InStack.insert(BB);
  do {
    std::pair<const BasicBlock*, succ_const_iterator> &Top = VisitStack.back();
    const BasicBlock *ParentBB = Top.first;
    succ_const_iterator &I = Top.second;

    bool FoundNew = false;
    while (I != succ_end(ParentBB)) {
      BB = *I++;
      if (Visited.insert(BB)) {
        FoundNew = true;
        break;
      }
      // Successor is in VisitStack, it's a back edge.
      if (InStack.count(BB))
        Result.push_back(std::make_pair(ParentBB, BB));
    }

    if (FoundNew) {
      // Go down one level if there is a unvisited successor.
      InStack.insert(BB);
      VisitStack.push_back(std::make_pair(BB, succ_begin(BB)));
    } else {
      // Go up one level.
      InStack.erase(VisitStack.pop_back_val().first);
    }
  } while (!VisitStack.empty());
}

/// GetSuccessorNumber - Search for the specified successor of basic block BB
/// and return its position in the terminator instruction's list of
/// successors.  It is an error to call this with a block that is not a
/// successor.
unsigned llvm::GetSuccessorNumber(BasicBlock *BB, BasicBlock *Succ) {
  TerminatorInst *Term = BB->getTerminator();
#ifndef NDEBUG
  unsigned e = Term->getNumSuccessors();
#endif
  for (unsigned i = 0; ; ++i) {
    assert(i != e && "Didn't find edge?");
    if (Term->getSuccessor(i) == Succ)
      return i;
  }
}

/// isCriticalEdge - Return true if the specified edge is a critical edge.
/// Critical edges are edges from a block with multiple successors to a block
/// with multiple predecessors.
bool llvm::isCriticalEdge(const TerminatorInst *TI, unsigned SuccNum,
                          bool AllowIdenticalEdges) {
  assert(SuccNum < TI->getNumSuccessors() && "Illegal edge specification!");
  if (TI->getNumSuccessors() == 1) return false;

  const BasicBlock *Dest = TI->getSuccessor(SuccNum);
  const_pred_iterator I = pred_begin(Dest), E = pred_end(Dest);

  // If there is more than one predecessor, this is a critical edge...
  assert(I != E && "No preds, but we have an edge to the block?");
  const BasicBlock *FirstPred = *I;
  ++I;        // Skip one edge due to the incoming arc from TI.
  if (!AllowIdenticalEdges)
    return I != E;

  // If AllowIdenticalEdges is true, then we allow this edge to be considered
  // non-critical iff all preds come from TI's block.
  while (I != E) {
    const BasicBlock *P = *I;
    if (P != FirstPred)
      return true;
    // Note: leave this as is until no one ever compiles with either gcc 4.0.1
    // or Xcode 2. This seems to work around the pred_iterator assert in PR 2207
    E = pred_end(P);
    ++I;
  }
  return false;
}

// LoopInfo contains a mapping from basic block to the innermost loop. Find
// the outermost loop in the loop nest that contains BB.
static const Loop *getOutermostLoop(const LoopInfo *LI, const BasicBlock *BB) {
  const Loop *L = LI->getLoopFor(BB);
  if (L) {
    while (const Loop *Parent = L->getParentLoop())
      L = Parent;
  }
  return L;
}

// True if there is a loop which contains both BB1 and BB2.
static bool loopContainsBoth(const LoopInfo *LI,
                             const BasicBlock *BB1, const BasicBlock *BB2) {
  const Loop *L1 = getOutermostLoop(LI, BB1);
  const Loop *L2 = getOutermostLoop(LI, BB2);
  return L1 != NULL && L1 == L2;
}

static bool isPotentiallyReachableInner(SmallVectorImpl<BasicBlock *> &Worklist,
                                        BasicBlock *StopBB,
                                        const DominatorTree *DT,
                                        const LoopInfo *LI) {
  // When the stop block is unreachable, it's dominated from everywhere,
  // regardless of whether there's a path between the two blocks.
  if (DT && !DT->isReachableFromEntry(StopBB))
    DT = 0;

  // Limit the number of blocks we visit. The goal is to avoid run-away compile
  // times on large CFGs without hampering sensible code. Arbitrarily chosen.
  unsigned Limit = 128;
  SmallSet<const BasicBlock*, 64> Visited;
  do {
    BasicBlock *BB = Worklist.pop_back_val();
    if (!Visited.insert(BB))
      continue;
    if (BB == StopBB)
      return true;
    if (DT && DT->dominates(BB, StopBB))
      return true;
    if (LI && loopContainsBoth(LI, BB, StopBB))
      return true;

    if (!--Limit) {
      // We haven't been able to prove it one way or the other. Conservatively
      // answer true -- that there is potentially a path.
      return true;
    }

    if (const Loop *Outer = LI ? getOutermostLoop(LI, BB) : 0) {
      // All blocks in a single loop are reachable from all other blocks. From
      // any of these blocks, we can skip directly to the exits of the loop,
      // ignoring any other blocks inside the loop body.
      Outer->getExitBlocks(Worklist);
    } else {
      Worklist.append(succ_begin(BB), succ_end(BB));
    }
  } while (!Worklist.empty());

  // We have exhausted all possible paths and are certain that 'To' can not be
  // reached from 'From'.
  return false;
}

bool llvm::isPotentiallyReachable(const BasicBlock *A, const BasicBlock *B,
                                  const DominatorTree *DT, const LoopInfo *LI) {
  assert(A->getParent() == B->getParent() &&
         "This analysis is function-local!");

  SmallVector<BasicBlock*, 32> Worklist;
  Worklist.push_back(const_cast<BasicBlock*>(A));

  return isPotentiallyReachableInner(Worklist, const_cast<BasicBlock*>(B),
                                     DT, LI);
}

bool llvm::isPotentiallyReachable(const Instruction *A, const Instruction *B,
                                  const DominatorTree *DT, const LoopInfo *LI) {
  assert(A->getParent()->getParent() == B->getParent()->getParent() &&
         "This analysis is function-local!");

  SmallVector<BasicBlock*, 32> Worklist;

  if (A->getParent() == B->getParent()) {
    // The same block case is special because it's the only time we're looking
    // within a single block to see which instruction comes first. Once we
    // start looking at multiple blocks, the first instruction of the block is
    // reachable, so we only need to determine reachability between whole
    // blocks.
    BasicBlock *BB = const_cast<BasicBlock *>(A->getParent());

    // If the block is in a loop then we can reach any instruction in the block
    // from any other instruction in the block by going around a backedge.
    if (LI && LI->getLoopFor(BB) != 0)
      return true;

    // Linear scan, start at 'A', see whether we hit 'B' or the end first.
    for (BasicBlock::const_iterator I = A, E = BB->end(); I != E; ++I) {
      if (&*I == B)
        return true;
    }

    // Can't be in a loop if it's the entry block -- the entry block may not
    // have predecessors.
    if (BB == &BB->getParent()->getEntryBlock())
      return false;

    // Otherwise, continue doing the normal per-BB CFG walk.
    Worklist.append(succ_begin(BB), succ_end(BB));

    if (Worklist.empty()) {
      // We've proven that there's no path!
      return false;
    }
  } else {
    Worklist.push_back(const_cast<BasicBlock*>(A->getParent()));
  }

  if (A->getParent() == &A->getParent()->getParent()->getEntryBlock())
    return true;
  if (B->getParent() == &A->getParent()->getParent()->getEntryBlock())
    return false;

  return isPotentiallyReachableInner(Worklist,
                                     const_cast<BasicBlock*>(B->getParent()),
                                     DT, LI);
}

static bool isPotentiallyReachableInnerNotViaDef(const BasicBlock *DefBB, SmallVectorImpl<BasicBlock *> &Worklist, const BasicBlock *SpBB,
                                        std::set<BasicBlock*>& EndBlocks,
                                        const DominatorTree *DT,
                                        const LoopInfo *LI) {

  // we can not conservatively include the use when timeout
  SmallSet<const BasicBlock*, 64> Visited;

  do {
    // This is a depth-first search for a path not via def
    // return true whenever the first valid path is found
    BasicBlock *BB = Worklist.pop_back_val();

    // if has already visited
    if (!Visited.insert(BB))
      continue;

    // Here we have 3 cases:
    // 1. If BB == SpBB, we just start the search from safepoint, and def should before safepoint in the same block, and BB == DefBB does not mean we reach the def in the search path.
    // 2. If DefBB == StopBB, it infers that use is a phi otherwise this case would be returned in a fastpath in isPotentiallyReachableNotViaDef().
    // and if use is a phi and in the same block with def, we reach use before def.
    // However, this case could not happen because if use is phi, it will stop at one of its incoming block and return true.
    // 3. If only BB == DefBB, we reach the def in the search path and this path should be discarded.
    if (BB == DefBB && BB != SpBB)
      continue;

    if (EndBlocks.find(BB) != EndBlocks.end())
      return true;

#ifdef OPTIMIZED_LIVENESS_ANALYSIS
    // If curent basicblock, the defblock, and any incoming block of PhiNode are not within a loop,
    // we can strip the loop and only add the loop exsit basicblock into the worklist to make the search shorter
    if (LI && (DefBB == NULL || !loopContainsBoth(LI, BB, DefBB))) {
      bool contains = false;
      for (std::set<BasicBlock*>::iterator itr = EndBlocks.begin(), end = EndBlocks.end(); itr != end; itr++) {
        if (loopContainsBoth(LI, BB, *itr)) {
          contains = true;
          break;
        }
      }
      if (!contains) {
        if (const Loop *Outer = getOutermostLoop(LI, BB)) {
          // All blocks in a single loop are reachable from all other blocks. From
          // any of these blocks, we can skip directly to the exits of the loop,
          // ignoring any other blocks inside the loop body.
          Outer->getExitBlocks(Worklist);
        }
      } else {
        Worklist.append(succ_begin(BB), succ_end(BB));
      }
    } else {
      Worklist.append(succ_begin(BB), succ_end(BB));
    }
#else
    Worklist.append(succ_begin(BB), succ_end(BB));
#endif
  } while (!Worklist.empty());

  // We have exhausted all possible paths and are certain that 'To' can not be
  // reached from 'From'.
  return false;
}

// This is a check to see if there is a path between the safepoint and use via the def
bool llvm::isPotentiallyReachableNotViaDef(Instruction *SP, Instruction *USE, Value *DEF,
                                  const DominatorTree *DT, const LoopInfo *LI) {
  assert(SP->getParent()->getParent() == USE->getParent()->getParent() &&
         "This analysis is function-local!");

  SmallVector<BasicBlock*, 32> Worklist;

  bool DefIsArg = isa<Argument>(DEF);

  BasicBlock *UseBlock = const_cast<BasicBlock*>(USE->getParent());
  BasicBlock *SpBlock = const_cast<BasicBlock*>(SP->getParent());

  // def could be an argument which has no basicblock
  BasicBlock *DefBlock = DefIsArg? NULL : const_cast<BasicBlock*>(cast<Instruction>(DEF)->getParent());

  // Port the hack on VMState in Alternative Liveness Analysis
  if( isStatepoint(USE) ) {
    StatepointOperands statepoint(USE);
    bool isNonDeoptArg = false;
    for(ImmutableCallSite::arg_iterator itr = statepoint.call_args_begin(), end = statepoint.call_args_end();
        itr != end; itr++) {
      if( *itr == DEF ) {
        isNonDeoptArg = true;
        break;
      }
    }
    for(ImmutableCallSite::arg_iterator itr = statepoint.gc_args_begin(), end = statepoint.gc_args_end();
        itr != end && !isNonDeoptArg; itr++) {
      if( *itr == DEF ) {
        isNonDeoptArg = true;
        break;
      }
    }
    if( !isNonDeoptArg ) {
      return false;
    }
  }

  // If use is live at the original call site which triggers the safepoint, it will be live during the call
  // This check should be enough to catch the case.
  // Backedge safepoint call is a pure "call void @do_safepoint()" and dose not have any use. It wont mix up with call safepoint.
  if (SP == USE)
    return true;

  std::set<BasicBlock*> EndBlocks;
  // If use is in PHINode, EndBlocks should be one of its incoming basic blocks
  if (isa<PHINode>(USE)) {
    const PHINode* phi = cast<PHINode>(USE);
    unsigned NumPHIValues = phi->getNumIncomingValues();
    for (unsigned i = 0; i != NumPHIValues; ++i) {
      Value *InVal = phi->getIncomingValue(i);
      if( InVal == DEF ) {
        EndBlocks.insert(phi->getIncomingBlock(i));
      }
    }
  } else {
    // If use is not in PHINode, Endblocks should be the blocks contains the use.
    EndBlocks.insert(UseBlock);
  }


  if (SpBlock == UseBlock) {
    // The same block case is special because it's the only time we're looking
    // within a single block to see which instruction comes first. Once we
    // start looking at multiple blocks, the first instruction of the block is
    // reachable, so we only need to determine reachability between whole
    // blocks.

    // Linear scan, start at safepoint, see whether we hit use or the end first.
    for (BasicBlock::const_iterator I = SP, E = SpBlock->end(); I != E; ++I) {
      if (&*I == USE)
        return true;
    }

    // If def is also in the same basicblock as safepoint and use (def must be before safepoint and use (when use is not a PHINode, and PHINode need to be treat specially) as it dominates them)
    // In the case, we know use occurs before safepoint (or it will be returned in the case above),
    // therefore safepoint can only reach use via a backedge to the begining of this basicblock and therefore has to pass def.
    if (!DefIsArg && DefBlock == UseBlock) {
        if (!isa<PHINode>(USE)) {
          return false;
        }
#ifdef OPTIMIZED_LIVENESS_ANALYSIS
        else { // If use is a PHINode, it should be at the beginning of the basicblock, we need to treat it specially and see if we reach the use via its incoming block
          // this is fastpath to see if there is a backedge directly from the current basicblock
          // and if it's the path of the use in PhiNode
          if (EndBlocks.find(SpBlock) != EndBlocks.end())
            return true;
        }
#endif
    }

    // Can't be in a loop if it's the entry block -- the entry block may not
    // have predecessors.
    if (SpBlock == &SpBlock->getParent()->getEntryBlock()) {
      assert((DefIsArg || DefBlock == SpBlock) && "Def does not dominate safepoint!");
      return false;
    }
    // Otherwise, continue doing the normal per-BB CFG walk.
    Worklist.append(succ_begin(SpBlock), succ_end(SpBlock));

    if (Worklist.empty()) {
      // We've proven that there's no path!
      return false;
    }
  } else {
    Worklist.push_back(SpBlock);
  }

  // If def use are in the same basicblock, and safepoint is in another basicblock
  // it's obvious that safepoint has to pass def before reaching use in the block containing both the def and use unless use is a PHINode.
  if (!DefIsArg && DefBlock == UseBlock) {
    if (!isa<PHINode>(USE)) {
      return false;
    } // If use is PHINode we cannot simply return true, we need to check if we pass the incoming block of the specific use in the PHINode
  }

  // This fastpath should only work if use is not in PHINode, otherwise we need to be path sensitive and can not rely on the fact
  // the safepoint block dominates all the other blocks.
#ifdef OPTIMIZED_LIVENESS_ANALYSIS
  // EntryBlock should dominate all blocks including the useblock,
  // also def dominates safepoint and therefore they should be in the same block or def is an argument
  if (SpBlock == &SpBlock->getParent()->getEntryBlock() && !isa<PHINode>(USE)) {
    assert((DefIsArg || DefBlock == SpBlock) && "Def does not dominate safepoint!");
    return true;
  }
#endif

  // The opposite as above if use is in the entryblock
  if (UseBlock == &SpBlock->getParent()->getEntryBlock()) {
    assert((DefIsArg || DefBlock == UseBlock) && "Def does not dominate use!");
    return false;
  }

  // When the stop block is unreachable, it's dominated from everywhere,
  // regardless of whether there's a path between the two blocks.
  if (DT && !DT->isReachableFromEntry(UseBlock))
    DT = 0;

  // This fastpath should only work if use is not in PHINode, otherwise we need to be path sensitive and can not rely on the fact
  // the safepoint block dominates all the other blocks.
#ifdef OPTIMIZED_LIVENESS_ANALYSIS
  // If safepoint dominates use (def should always dominate safepoint), we cannot reach use via def.
  if (DT && DT->dominates(SP, USE) && !isa<PHINode>(USE))
    return true;
#endif

  return isPotentiallyReachableInnerNotViaDef(DefBlock, Worklist, SpBlock,
                                     EndBlocks,
                                     DT, LI);
}

