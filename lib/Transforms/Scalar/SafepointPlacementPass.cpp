/** Place safepoints in appropriate locations in the IR.  The two interesting
    locations are at call sites (which require stack parsability and VM state
    for deopt) and loop backedges (which require all of that, plus actual poll
    for the safepoint.)

    Note: This is a proof of concept implementation.  It is slow, and has a few
    known bugs in corner cases (see the failures/ test dir).  Use at your own risk.

    TODO: Documentation
    Explain why the various algorithms work.  Justify the pieces in at least
    high level detail.    
 */
#define DEBUG_TYPE "safepoint-erasure"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/JVMState.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;
using namespace std;

// Set this debugging variable to 1 to have the IR verified for consistency
// after every transform is complete.
cl::opt<bool> VerifyAllIR ("spp-verify-all-ir", cl::init(true));

// Ignore oppurtunities to avoid placing safepoints on backedges, useful for validation
cl::opt<bool> AllBackedges ("spp-all-backedges", cl::init(false));
// Only go as far as confirming base pointers exist, useful for fault isolation
cl::opt<bool> BaseRewriteOnly ("spp-base-rewrite-only", cl::init(false) );
// Add safepoints to all functions, not just the ones with attributes
cl::opt<bool> AllFunctions ("spp-all-functions", cl::init(false) );
// Include deopt state in safepoints?
cl::opt<bool> UseVMState ("spp-use-vm-state", cl::init(true) );

// Should we enable the known base optimizations?  It can be useful to turn
// this off to stress relocation logic.
cl::opt<bool> EnableKnownBaseOpt ("spp-known-base-opt", cl::init(true) );

// Print tracing output
cl::opt<bool> TraceLSP ("spp-trace", cl::init(false) );

// Print the liveset found at the insert location
cl::opt<bool> PrintLiveSet ("spp-print-liveset", cl::init(false));
cl::opt<bool> PrintLiveSetSize ("spp-print-liveset-size", cl::init(false));
// Print out the base pointers for debugging
cl::opt<bool> PrintBasePointers("spp-print-base-pointers", cl::init(false));


// Bugpoint likes to reduce a crash into _any_ crash (including assertion
// failures due to configuration problems).  If we're reducing a 'real' crash
// under bugpoint, make simple configuration errors (which bugpoint introduces)
// look like normal behavior.
#define USING_BUGPOINT
#ifdef USING_BUGPOINT
#define BUGPOINT_CLEAN_EXIT_IF( cond ) if(true) { \
    if((cond)) { errs() << "FATAL ERROR, exit cleanly for bugpoint\n"; exit(0); } else {}  \
  } else {}
#else
#define BUGPOINT_CLEAN_EXIT_IF( cond ) if(true) {   \
  } else {}
#endif

static bool VMStateRequired() {
  return !AllFunctions && UseVMState;
}


/* Note: Both PlaceBackedgeSafepoints and PlaceEntrySafepoints need to be
   instances of ModulePass, not FunctionPass.  FunctionPass is not allowed to
   do any cross module optimization (such as inlining).  The PassManager will
   run FunctionPasses in "some order" on all the relevant functions.  In
   theory, the gc.safepoint_poll function could be being optimized
   (i.e. potentially invalid IR) when we attempt to inline it.  While in
   practice, passes aren't run in parallel, we did see an issue where we'd
   insert a safepoint into the poll function and *then* inline it.  Yeah,
   inlining after safepoint placement is utterly completely illegal and wrong.

   The only reason this works today is that a) we manually exclude
   safepoint_poll from consideration even under AllFunctions and b) we have
   barrier passes immediately before and after safepoint insertion.  This still
   isn't technically enough (LCSSA can modify loop edges in certian
   poll_safepoints, who knew?), but it mostly appears to work for the moment.

   THIS REALLY NEEDS FIXED.
 */

namespace {

  // TODO: This should not be a loop pass.  Merge with upstream LLVM changes
  // for LCCSA and LoopSimplify, then make it a module pass
struct PlaceBackedgeSafepoints : public LoopPass {
  static char ID;
  PlaceBackedgeSafepoints()
    : LoopPass(ID) {
    initializePlaceBackedgeSafepointsPass(*PassRegistry::getPassRegistry());
  }

  virtual bool runOnLoop(Loop *, LPPassManager &LPM);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    // Be careful before you start removing these - I got some odd crashes when
    // I removed analysis I wasn't obviously using
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfo>();
    AU.addRequired<ScalarEvolution>();
    AU.addRequiredID(LoopSimplifyID);
    // LCSSA is a special form which ensures that values defined in the loop
    // which survive the loop have a phi node in the exit block.  
    AU.addRequiredID(LCSSAID);

    AU.addPreserved<DominatorTreeWrapperPass>();
    //AU.setPreservesCFG(); // We insert new blocks!
    //Q: Do we preserve any others?  I need to think about this a bit.
    //insertPHIs in particular may break LoopSimplify and/or LCSSA
  }

};

struct PlaceCallSafepoints : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  PlaceCallSafepoints() : FunctionPass(ID) {
    initializePlaceCallSafepointsPass(*PassRegistry::getPassRegistry());
  }
  virtual bool runOnFunction(Function &F) {

    bool shouldRun = AllFunctions ||
      F.getFnAttribute("gc-add-call-safepoints").getValueAsString().equals("true");
    if( shouldRun && F.getName().equals("gc.safepoint_poll") ) {
      assert(AllFunctions && "misconfiguration");
      //go read the module pass comment above
      shouldRun = false;
      errs() << "WARNING: Ignoring (illegal) request to place safepoints in gc.safepoint_poll\n";
    }
    if( !shouldRun ) {
      return false;
    }
    
    bool modified = false;
    for(Function::iterator itr = F.begin(), end = F.end();
        itr != end; itr++) {
      BasicBlock& BB = *itr;
      modified |= runOnBasicBlock(BB);
    }
    return modified;
  }
  virtual bool runOnBasicBlock(BasicBlock &BB);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DominatorTreeWrapperPass>();

    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.setPreservesCFG();
  }

};
  // Needs to be a module pass - it can do inlining!
struct PlaceEntrySafepoints : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  PlaceEntrySafepoints() : FunctionPass(ID) {
    initializePlaceEntrySafepointsPass(*PassRegistry::getPassRegistry());
  }
  virtual bool runOnFunction(Function &F);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DominatorTreeWrapperPass>();

    AU.addPreserved<DominatorTreeWrapperPass>();
    // We insert blocks!
    // AU.setPreservesCFG();
  }

};

struct RemoveFakeVMStateCalls : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  RemoveFakeVMStateCalls() : FunctionPass(ID) {
    initializeRemoveFakeVMStateCallsPass(*PassRegistry::getPassRegistry());
  }
  virtual bool runOnFunction(Function &F) {
    // Track the calls and function definitions to be removed
    std::vector<CallInst*> instToRemove;
    std::set<Function*> funcToRemove;
    for( inst_iterator itr = inst_begin(F), end = inst_end(F);
         itr != end; itr++) {
      if (isJVMState(&*itr)) {
        CallInst *CI = cast<CallInst>(&*itr);
        instToRemove.push_back(CI);
        funcToRemove.insert(CI->getCalledFunction());
      }
    }

    //remove all the calls (i.e. uses of functions)
    for(size_t i = 0; i < instToRemove.size(); i++) {
      CallInst* CI = instToRemove[i];

      // Remove the use holding this call in place
      assert( std::distance(CI->use_begin(), CI->use_end()) == 1 && "must have exactly one use");
      StoreInst* use = cast<StoreInst>(*CI->use_begin());
      assert(isJVMStateAnchorInstruction(use));
      use->eraseFromParent();
      assert( std::distance(CI->use_begin(), CI->use_end()) == 0 && "should be no uses left");

      // Remove the call itself
      CI->eraseFromParent();
      instToRemove[i] = NULL;
    }

    // remove the functions which are now dead - note that the use of a set is
    // required since calls can be duplicated by the optimizer
    for(std::set<Function*>::iterator itr = funcToRemove.begin(), end = funcToRemove.end();
        itr != end; itr++) {
      Function* F = *itr;
      // The conditional is a safety check to handle another use which is
      // somehow hanging around.
      if( F->use_empty() ) {
        F->eraseFromParent();
      }
    }
    return true;
  }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesCFG();
  }
};
}


// The following declarations call out the key steps of safepoint placement and
// summarize their preconditions, postconditions, and side effects.  This is
// best read as a summary; if you need detail on implementation, dig into the
// actual implementations below.
namespace SafepointPlacementImpl {

  // Insert a safepoint (parse point) at the given call instruction.
  void InsertSafepoint(DominatorTree& DT, const CallSite& CS, CallInst* vmstate);
  // Insert a safepoint poll immediately before the given instruction.  Does
  // not provide a parse point for the 'after' instruction itself.  Updates the
  // DT and Loop to reflect changes.  Loop can be null.
  void InsertSafepointPoll(DominatorTree& DT, LoopInfo* LI, Loop* L, Instruction* after, CallInst* vmstate);

  bool isGCLeafFunction(const CallSite& CS);

  bool needsStatepoint(const CallSite& CS) {
    if( isGCLeafFunction(CS) ) return false;
    if( CS.isCall() ) {
      // Why the hell is inline ASM modeled as a call instruction?
      CallInst* call = cast<CallInst>(CS.getInstruction());
      if( call->isInlineAsm() ) return false;
    }
    if( isStatepoint(CS) || isGCRelocate(CS) || isGCResult(CS) ) {
      // In case we run backedge, then call safepoint placement...
      return false;
    }
    return true;
  }

  bool isLiveAtSafepoint(Instruction* term, Instruction* use, Value& inst, DominatorTree& DT, LoopInfo* LI);
  
  /** Returns an overapproximation of the live set for entry of a given
      instruction. The liveness analysis is performed immediately before the 
      given instruction. Values defined by that instruction are not considered
      live.  Values used by that instruction are considered live. Note that the 
      use of the term Value is intentional. Arguments and other non variable
      non instruction Values can be live.

      preconditions: valid IR graph, term is either a terminator instruction or
      a call instruction, pred is the basic block of term, DT, LI are valid

      side effects: none, does not mutate IR

      postconditions: populates liveValues as discussed above
  */
  void findLiveGCValuesAtInst(Instruction* term, BasicBlock* pred, DominatorTree& DT, LoopInfo* LI, std::set<llvm::Value*>& liveValues);

  /** For a set of live pointers (base and/or derived), identify the base
      pointer of the object which they are derived from.  This routine will
      mutate the IR graph as needed to make the 'base' pointer live at the
      definition site of 'derived'.  This ensures that any use of 'derived' can
      also use 'base'.  This may involve the insertion of a number of
      additional PHI nodes.  
      
      preconditions: live is a set of pointer type Values, all arguments are
      base pointers, all globals are base pointers, any gc pointer value in
      the heap is a base pointer.  

      side effects: may insert PHI nodes into the existing CFG, will preserve
      CFG, will not remove or mutate any existing nodes
      
      post condition: base_pairs contains one (derived, base) pair for every
      pointer in live.  Note that derived can be equal to base if the original
      pointer was a base pointer.
  */
  void findBasePointers(const std::set<llvm::Value*>& live,
                        std::map<llvm::Value*, llvm::Value*>& base_pairs,
                        DominatorTree *DT, std::set<llvm::Value*>& newInsertedDefs);

  CallInst* findVMState(llvm::Instruction* term, DominatorTree *DT);

  /** Inserts the actual code for a safepoint.  Currently this inserts a
      statepoint, gc_relocate(*) series, but that could change easily.  The
      resulting new definitions (SSA values) are returned via reference.  The
      result vector will exactly align with the vector of pointer values passed
      in.  The safepoints are inserted immediately before the specified
      instruction.

      Returns a pair which describes the range of code inserted.  Format is
      [first, last] (i.e. inclusive, not exclusive)

      WARNING: Does not do any fixup to adjust users of the original live
      values.  That's the callers responsibility.

      pre: valid IR, all Values in liveVariables are live at insertBefore

      side effects: inserts new IR for safepoint, does not delete or mutate
      nodes, preserves CFG

      post: valid IR which does not respect the newly inserted safepoint.
      length(live) == length(newDefs) and all new/old values are aligned.
  */
  std::pair<Instruction*, Instruction*>
  CreateSafepoint(const CallSite& CS, /* to replace */
                  llvm::CallInst* vm_state,
                  const std::vector<llvm::Value*>& basePtrs,
                  const std::vector<llvm::Value*>& liveVariables,
                  std::vector<llvm::Instruction*>& newDefs);
  
  /// This routine walks the CFG and inserts PHI nodes as needed to handle a
  /// new definition which is replacing an old definition at a location where
  /// there didn't use to be a use.  The Value being replaced need not be an
  /// instruction (it can be an alloc, or argument for instance), but the
  /// replacement definition must be an Instruction.
  void insertPHIsForNewDef(DominatorTree& DT, Value* oldDef, Instruction* newDef, std::pair<Instruction*, Instruction*> safepoint);
}

using namespace SafepointPlacementImpl;

namespace {
  /// Returns true if this loop is known to terminate in a finite number of
  /// iterations.  Note that this function may return false for a loop which
  /// does actual terminate in a finite constant number of iterations due to
  /// conservatism in the analysis.
  bool mustBeFiniteCountedLoop(Loop* L, ScalarEvolution* SE) {
    unsigned TripCount = 0;
    
    //Currently, only handles loops with a single backedge (common case)
    // Note: Due to LoopSimplify dependency, all loops we see should be in this
    // form. The only exception would be indirectbr which we disallow.
    BasicBlock *LatchBlock = L->getLoopLatch();
    if (LatchBlock) {
      // Will return a finite number which bounds the trip count through the
      // latch block.  Since the latch block is the only backedge, this bounds
      // the number of iterations from above
      TripCount = SE->getSmallConstantTripCount(L, LatchBlock);
    }
    // TODO: it would be trivial to use a more restricted definition for pruning
    // safepoints.  Maybe loops with trip counts less than some finite number?
    return TripCount > 0;
  }

  void addBasesAsLiveValues(std::set<Value*>& liveset,
                          std::map<Value*, Value*>& base_pairs) {
    // Identify any base pointers which are used in this safepoint, but not
    // themselves relocated.  We need to relocate them so that later inserted
    // safepoints can get the properly relocated base register.
    std::set<Value*> missing;
    for(std::set<llvm::Value*>::iterator itr = liveset.begin(), E = liveset.end();
        itr != E; itr++) {
      assert( base_pairs.find(*itr) != base_pairs.end() );
      Value* base = base_pairs[ *itr ];
      assert( base );
      if( liveset.find( base ) == liveset.end() ) {
        assert( base_pairs.find( base ) == base_pairs.end() );
        //uniqued by set insert
        missing.insert(base);
      }
    }

    // Note that we want these at the end of the list, otherwise
    // register placement gets screwed up once we lower to STATEPOINT
    // instructions.  This is an utter hack, but there doesn't seem to be a
    // better one.
    for(std::set<llvm::Value*>::iterator itr = missing.begin(), E = missing.end();
        itr != E; itr++) {
      Value* base = *itr;
      assert( base );
      liveset.insert(base);
      base_pairs[base] = base;
    }
    assert( liveset.size() == base_pairs.size() );
  }
  void scanOneBB(Instruction* start, Instruction* end,
                 std::vector<CallInst*>& calls,
                 std::set<BasicBlock*>& seen,
                 std::vector<BasicBlock*>& worklist) {
    for( BasicBlock::iterator itr(start);
         itr != start->getParent()->end() && itr != BasicBlock::iterator(end); itr++) {
      if( CallInst* CI = dyn_cast<CallInst>(&*itr) ) {
        calls.push_back(CI);
      }
      // FIXME: This code does not handle invokes
      assert( !dyn_cast<InvokeInst>(&*itr) && "support for invokes in poll code needed");
      // Only add the successor blocks if we reach the terminator instruction
      // without encountering end first
      if( itr->isTerminator() ) {
        BasicBlock* BB = itr->getParent();
        for (succ_iterator PI = succ_begin(BB), E = succ_end(BB); PI != E; ++PI) {
          BasicBlock *Succ = *PI;
          if( seen.count(Succ) == 0 ) {
            worklist.push_back(Succ);
            seen.insert(Succ);
          }
        }
      }
    }
  }
  void scanInlinedCode(Instruction* start, Instruction* end,
                       std::vector<CallInst*>& calls, std::set<BasicBlock*>& seen) {
    calls.clear();
    std::vector<BasicBlock*> worklist;
    seen.insert( start->getParent() );
    scanOneBB(start, end, calls,  seen, worklist);
    while( !worklist.empty() ) {
      BasicBlock* BB = worklist.back();
      worklist.pop_back();
      scanOneBB(&*BB->begin(), end, calls, seen, worklist);
    }
  }

}

bool PlaceBackedgeSafepoints::runOnLoop(Loop* L, LPPassManager &LPM) {
  LoopInfo *LI = &getAnalysis<LoopInfo>();
  ScalarEvolution *SE = &getAnalysis<ScalarEvolution>();
  DominatorTreeWrapperPass& DTWP = getAnalysis<DominatorTreeWrapperPass>();
  DominatorTree& DT = DTWP.getDomTree();


  // Loop through all predecessors of the loop header and identify all
  // backedges.  We need to place a safepoint on every backedge (potentially).
  // Note: Due to LoopSimplify there should only be one.  Assert?  Or can we
  // relax this?
  BasicBlock* header = L->getHeader();

  Function* Func = header->getParent();
  assert(Func);
  bool shouldRun = AllFunctions ||
    Func->getFnAttribute("gc-add-backedge-safepoints").getValueAsString().equals("true");
  if( shouldRun && Func->getName().equals("gc.safepoint_poll") ) {
    assert(AllFunctions && "misconfiguration");
    //go read the module pass comment above
    shouldRun = false;
    errs() << "WARNING: Ignoring (illegal) request to place safepoints in gc.safepoint_poll\n";
  }
  if( !shouldRun ) {
    return false;
  }


  if( VerifyAllIR ) {
    // precondition check
    verifyFunction(*header->getParent());
    verifySafepointIR(*header->getParent());
  }

  bool modified = false;
  for( pred_iterator PI = pred_begin(header), E = pred_end(header); PI != E; PI++ ) {
    BasicBlock* pred = *PI;
    if( !L->contains(pred) ) {
      // This is not a backedge, it's coming from outside the loop
      continue;
    }

    // Make a policy decision about whether this loop needs a safepoint or
    // not.  Place early for performance.  Could run later for some validation,
    // but at great cost performance wise.
    if( !AllBackedges ) {
      if( mustBeFiniteCountedLoop(L, SE) ) {
        if( TraceLSP ) 
          errs() << "skipping safepoint placement in finite loop\n";
        continue;
      }

      // TODO: if the loop already contains a call safepoint, no backedge
      // safepoint needed
    }

    // We're unconditionally going to modify this loop.
    modified = true;

    // Safepoint insertion would involve creating a new basic block (as the
    // target of the current backedge) which does the safepoint (of all live
    // variables) and branches to the true header
    TerminatorInst * term = pred->getTerminator();

    if (TraceLSP) {
      errs() << "[LSP] terminator instruction: "; term->dump();
    }

    // locate the defining VM state object for this location
    CallInst* vm_state = NULL;
    if( VMStateRequired() ) {
      vm_state = findVMState(term, &DT);
      BUGPOINT_CLEAN_EXIT_IF( !vm_state );
      assert( vm_state && "must find vm state or be scanning c++ source code");
    }

    InsertSafepointPoll(DT, LI, L, term, vm_state);
  }

  return modified;
}

bool PlaceCallSafepoints::runOnBasicBlock(BasicBlock &BB) {
  // Note: We do this in two passes to avoid reasoning about invalidate
  // iterators since we replace the call instruction and erase the old ones
  std::vector<CallSite> toUpdate;
  for(BasicBlock::iterator institr = BB.begin(), end = BB.end();
      institr != end; institr++) {
    Instruction& inst = *institr;

    if( isa<CallInst>(&inst) || isa<InvokeInst>(&inst) ) {
      CallSite CS(&inst);

      // No safepoint needed or wanted
      if( !needsStatepoint(CS) ) {
        continue;
      }
      
      toUpdate.push_back(CS);
    }
  }
  DominatorTreeWrapperPass& DTWP = getAnalysis<DominatorTreeWrapperPass>();
  DominatorTree& DT = DTWP.getDomTree();
  for(size_t i = 0; i < toUpdate.size(); i++) {
    CallSite& CS = toUpdate[i];

    // locate the defining VM state object for this location
    CallInst* vm_state = NULL;
    if( VMStateRequired() ) {
      vm_state = findVMState(CS.getInstruction(), &DT);
      BUGPOINT_CLEAN_EXIT_IF( !vm_state );
      assert( vm_state && "must find vm state or be scanning c++ source code");
    }

    // Note: This deletes the instruction refered to by the CallSite!
    InsertSafepoint(DT, CS, vm_state);

    if( VerifyAllIR ) {
      // between every safepoint, and post condition
      verifyFunction(*BB.getParent());
      verifySafepointIR(*BB.getParent());
    }
  }

  return !toUpdate.empty();
}

bool PlaceEntrySafepoints::runOnFunction(Function &F) {
  DominatorTreeWrapperPass& DTWP = getAnalysis<DominatorTreeWrapperPass>();
  DominatorTree& DT = DTWP.getDomTree();
  
  bool shouldRun = AllFunctions ||
    F.getFnAttribute("gc-add-entry-safepoints").getValueAsString().equals("true");
  if( shouldRun && F.getName().equals("gc.safepoint_poll") ) {
    assert(AllFunctions && "misconfiguration");
    //go read the module pass comment above
    shouldRun = false;
    errs() << "WARNING: Ignoring (illegal) request to place safepoints in gc.safepoint_poll\n";
  }
  if( !shouldRun ) {
    return false;
  }

  // Conceptually, this poll needs to be on method entry, but in practice, we
  // place it as late in the entry block as possible.  We need to be after the
  // first BCI (to have a valid VM state), but there's no reason we can't be
  // arbitrarily late.  The location simply needs to dominate all the returns.
  // This is required to ensure bounded time to safepoint in the face of
  // recursion.
  // PERF: Can we avoid this for non-recursive functions?
  // PERF: Don't emit if call guaranteed to occur

  if (F.begin() == F.end()) {
    // Empty function, nothing was done.
    return false;
  }

  // Due to the way the frontend generates IR, we may have a couple of intial
  // basic blocks before the first bytecode.  These will be single-entry
  // single-exit blocks which conceptually are just part of the first 'real
  // basic block'.  Since we don't have deopt state until the first bytecode,
  // walk forward until we've found the first unconditional branch or merge.
  // Technically, we only 'need' to walk forward until we can find a VMState,
  // but a) that creates differences in placement between deopt enabled and not
  // (which complicates reduction and debugging) and b) the further in we are
  // the less live variables there are likely to be.  So we'll walk as far as
  // we can.  
  BasicBlock *currentBB = &F.getEntryBlock();
  while (true) {
    BasicBlock* nextBB = currentBB->getUniqueSuccessor();
    if( !nextBB ) {
      // split node
      break;
    }
    if( NULL == nextBB->getUniquePredecessor() ) {
      // next node is a join node, stop here
      // PERF: There's technically no correctness reason we need to stop here.
      // We mostly stop to avoid weird looking situations like having an
      // 'entry' safepoint in the middle of a loop before a backedge. It might
      // be worth checking how performance changes if we allow this to flow
      // farther in.  Particularly if we combined this with some form of
      // redudant safepoint removal (i.e. if there's already a backedge
      // safepoint and that post dominates the entry, why insert a method entry
      // safepoint at all?)
      break;
    }
    currentBB = nextBB;
  }
  TerminatorInst* term = currentBB->getTerminator();
  
  CallInst *vm_state = NULL;
  if( VMStateRequired() ) {
    vm_state = findVMState(term, &DT);
    BUGPOINT_CLEAN_EXIT_IF( !vm_state );
    assert( vm_state && "must find vm state or be scanning c++ source code");
  }

  InsertSafepointPoll(DT, NULL, NULL /*no loop*/, term, vm_state);
  return true; // we modified the CFG because we inserted a safepoint
}


char PlaceBackedgeSafepoints::ID = 0;
char PlaceCallSafepoints::ID = 0;
char PlaceEntrySafepoints::ID = 0;
char RemoveFakeVMStateCalls::ID = 0;

Pass *llvm::createPlaceBackedgeSafepointsPass() {
  return new PlaceBackedgeSafepoints();
}
FunctionPass *llvm::createPlaceCallSafepointsPass() {
  return new PlaceCallSafepoints();
}
FunctionPass *llvm::createPlaceEntrySafepointsPass() {
  return new PlaceEntrySafepoints();
}

FunctionPass *llvm::createRemoveFakeVMStateCallsPass() {
  return new RemoveFakeVMStateCalls();
}

INITIALIZE_PASS_BEGIN(PlaceBackedgeSafepoints,
                "place-backedge-safepoints", "Place Backedge Safepoints", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(LCSSA)
INITIALIZE_PASS_END(PlaceBackedgeSafepoints,
                "place-backedge-safepoints", "Place Backedge Safepoints", false, false)

INITIALIZE_PASS_BEGIN(PlaceCallSafepoints,
                "place-call-safepoints", "Place Call Safepoints", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(PlaceCallSafepoints,
                    "place-call-safepoints", "Place Call Safepoints", false, false)


INITIALIZE_PASS_BEGIN(PlaceEntrySafepoints,
                "place-entry-safepoints", "Place Method Enty Safepoints", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(PlaceEntrySafepoints,
                    "place-entry-safepoints", "Place Method Entry Safepoints", false, false)

INITIALIZE_PASS_BEGIN(RemoveFakeVMStateCalls,
                "remove-fake-vmstate-calls", "Remove VM state calls", false, false)
INITIALIZE_PASS_END(RemoveFakeVMStateCalls,
                    "remove-fake-vmstate-calls", "Remove VM state calls", false, false)

bool SafepointPlacementImpl::isGCLeafFunction(const CallSite& CS) {
  Instruction* inst = CS.getInstruction();
  if( IntrinsicInst *II = dyn_cast<IntrinsicInst>(inst) ) {
    switch(II->getIntrinsicID()) {
    default:
      // Most LLVM intrinsics are things which can never take a safepoint.
      // As a result, we don't need to have the stack parsable at the
      // callsite.  This is a highly useful optimization since intrinsic
      // calls are fairly prevelent, particularly in debug builds.
      return true;
    case Intrinsic::memset:
    case Intrinsic::memmove:
    case Intrinsic::memcpy:
      // These are examples of routines where we're going to override their
      // implementations and we do want them to have safepoints internally.
      // We may need to add others later.
      break; //fall through to generic call handling
    }
  }

  // If this function is marked explicitly as a leaf call, we don't need to
  // place a safepoint of it.  In fact, for correctness we *can't* in many
  // cases.  Note: Indirect calls return Null for the called function,
  // these obviously aren't runtime functions with attributes
  const Function* F = CS.getCalledFunction();
  bool isLeaf = F && F->getFnAttribute("gc-leaf-function").getValueAsString().equals("true");
  if(isLeaf) {
    return true;
  }
  return false;
}


void SafepointPlacementImpl::InsertSafepointPoll(DominatorTree& DT, LoopInfo* LI, Loop* L, Instruction* term, CallInst* vm_state) {
  assert( !vm_state || term->getParent()->getParent() == vm_state->getParent()->getParent() &&
          "must belong to the same function");
  
  Module* M = term->getParent()->getParent()->getParent();
  assert(M);  

  // Inline the safepoint poll implementation - this will get all the branch,
  // control flow, etc..  Most importantly, it will introduce the actual slow
  // path call - where we need to insert a safepoint (parsepoint).
  FunctionType* ftype = FunctionType::get(Type::getVoidTy(M->getContext()), false);
  assert(ftype && "null?");
  // Note: This cast can fail if there's a function of the same name with a
  // different type inserted previously
  Function* F = dyn_cast<Function>(M->getOrInsertFunction("gc.safepoint_poll", ftype));
  BUGPOINT_CLEAN_EXIT_IF( !(F && F->begin() != F->end()));
  assert( F && F->begin() != F->end() && "definition must exist");
  CallInst* poll = CallInst::Create(F, "", term);
  
  if( VerifyAllIR ) { verifyFunction(*term->getParent()->getParent()); }
  
  // Record some information about the call site we're replacing
  BasicBlock* OrigBB = term->getParent();
  BasicBlock::iterator before(poll), after(poll);
  bool isBegin(false);
  if( before == term->getParent()->begin() ) {
    isBegin = true;
  } else {
    before--;
  }
  after++;
  assert( after != poll->getParent()->end() && "must have successor");
  assert( DT.dominates(before, after) && "trivially true");
  
  // do the actual inlining
  InlineFunctionInfo IFI;
  bool inlineStatus = InlineFunction(poll, IFI);
  assert(inlineStatus && "inline must succeed");
  
  // Check post conditions
  assert( IFI.StaticAllocas.empty() && "can't have allocs");
  
  std::vector<CallInst*> calls; // new calls
  std::set<BasicBlock*> BBs; //new BBs + insertee
  // Include only the newly inserted instructions, Note: begin may not be valid
  // if we inserted to the beginning of the basic block
  BasicBlock::iterator start;
  if(isBegin) { start = OrigBB->begin(); }
  else { start = before; start++; }

  // If your poll function includes an unreachable at the end, that's not
  // valid.  Bugpoint likes to create this, so check for it.
  BUGPOINT_CLEAN_EXIT_IF( !isPotentiallyReachable(&*start, &*after, NULL, NULL) );
  assert( isPotentiallyReachable(&*start, &*after, NULL, NULL) &&
          "malformed poll function");
  
  scanInlinedCode(&*(start), &*(after), calls, BBs);
  
  // Recompute since we've invalidated cached data.  Conceptually we
  // shouldn't need to do this, but implementation wise we appear to.  Needed
  // so we can insert safepoints correctly.
  DT.recalculate(*after->getParent()->getParent());

  // Not all callers are within loops.
  if( L && LI ) {
    // Update the loop information so that we don't trip over verifyLoop
    // in the LPM
    for(std::set<BasicBlock*>::iterator itr = BBs.begin();
        itr != BBs.end(); itr++) {
      // The second condition is to avoid blocks which are essentially side
      // exits.  Normal return or exceptional flow would each have an in loop
      // sucessor, but a block which simply terminates execution would not.
      if( !L->contains(*itr) && !isa<UnreachableInst>((*itr)->getTerminator())) {
        L->addBasicBlockToLoop(*itr, LI->getBase());
      } 
    }
  }
    
  if( VerifyAllIR ) { verifyFunction(*term->getParent()->getParent()); }

  BUGPOINT_CLEAN_EXIT_IF(calls.empty());
  assert( !calls.empty() && "slow path not found for safepoint poll");
  
  // Add a call safepoint for each newly inserted call, this is required so
  // that the runtime knows how to parse the last frame when we actually take
  // the safepoint (i.e. execute the slow path)
  for(size_t i = 0; i< calls.size(); i++) {

    // No safepoint needed or wanted
    if( !needsStatepoint(calls[i]) ) {
      continue;
    }
    
    // These are likely runtime calls.  Should we assert that via calling
    // convention or something?
    InsertSafepoint(DT, CallSite(calls[i]), vm_state);

    if( VerifyAllIR ) {
      // between each
      verifyFunction(*term->getParent()->getParent());
      verifySafepointIR(*term->getParent()->getParent());
    }
  }
  if( VerifyAllIR ) {
    // post condition
    verifyFunction(*term->getParent()->getParent());
    verifySafepointIR(*term->getParent()->getParent());
  }
}


namespace {

  bool isRelocationPhiOfX(PHINode* phi, Value* X) {
    if( !phi->getMetadata("is_relocation_phi") ) {
      return false;
    }
    const unsigned NumPHIValues = phi->getNumIncomingValues();
    assert( NumPHIValues > 0 && "zero input phis are illegal");
    for (unsigned i = 0; i != NumPHIValues; ++i) {
      Value *InVal = phi->getIncomingValue(i);
      if( InVal == X ) {
        return true;
      }
    }
    return false;
  }

  // Walk backwards until we find the currently relocation of base.  When we
  // haven't inserted safepoints yet, this will simply be base.  Otherwise, it
  // will be the nearest relocation phi.  This is neccessary since the
  // optimistic base computation can find a base value which is common along
  // all paths in a previous existing relocation phi.  Picture:
  //   SP <-- our base points here
  //  / \
  //  \ /
  // rel phi <-- we skip this rel phi
  //  / \
  //  \ /
  //  SP <-- this is the new safepoint
  // Note: This function is known to be buggy.  See test case in failures/
  Value* findCurrentRelocationForBase(Instruction* term, Value* base) {
    // If base is a Constant, it will not equal to any instruction in the later check
    // and it should simply return itself.
    if (isa<Constant>(base)) {
      Constant* temp = cast<Constant>(base);
      assert( !isa<GlobalVariable>(base) && !isa<UndefValue>(base) && "order of checks wrong!");
      assert( temp->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      assert( temp->isNullValue() && "null is the only case which makes sense");
      return base;
    }
    
    // Walk back to the first join node - scanning each block as we encounter it
    BasicBlock* BB = term->getParent();
    while(true) {
      BasicBlock::reverse_iterator itr = BB->rbegin();
      if( BB == term->getParent() ) {
        itr = BasicBlock::reverse_iterator(term);
      }
      for( BasicBlock::reverse_iterator end = BB->rend();
           itr != end; itr++) {
        Instruction* inst = &*itr;
        if( inst == base ) {
          // found the def, it is current
          return base;
        }

        assert( !isStatepoint(inst) && "base pointer was not live at last safepoint" );
      }
      if( 1 != std::distance(pred_begin(BB), pred_end(BB)) ) {
        // We've found the first join block or the entry block
        break;
      }
      BB = BB->getSinglePredecessor();
    }

    if( BB != &BB->getParent()->getEntryBlock() &&
        std::distance(pred_begin(BB), pred_end(BB)) <= 0 ) {
      BUGPOINT_CLEAN_EXIT_IF(true);
      assert( false &&
              "don't support unreachable basic blocks for insertion");
    }

    // We've already scanned the block itself, so now we look for terminating
    // conditions.  For the entry block, this means arguments, for every other
    // block (which must be join) this means relocation phis

    if( BB == &BB->getParent()->getEntryBlock() ) {
      if( isa<Argument>(base) ) {
        // definition still valid
        return base;
      }
      
      llvm_unreachable("we should have found the def!");
    }
    
    bool relocationPhiFound = false;
    for(BasicBlock::iterator itr = BB->begin(), end = BB->getFirstNonPHI();
        itr != end; itr++) {
      PHINode* phi = cast<PHINode>(&*itr);
      if( phi->getMetadata("is_relocation_phi") ) {
        relocationPhiFound = true;
        if( isRelocationPhiOfX(phi, base) ) {
          // We found a relocaiton phi, this must be the currently live version
          // of the original base pointer.
          return phi;
        }
      }
    }
    if( relocationPhiFound ) {
      // If relocation phi is found but base is not in any of the relocation phi
      // the only valid case is that the base is a new def after last safepoint.
      // If not, it should be caught by the safepointIRVerifier later.
      // Therefore we choose not to duplicate the verification here, despite this
      // we can easily check for a few clear errors here to help isolate problems quickly
      assert(!isa<Argument>(base) && "If an argument is not live at last safepoint, it should not reach here across the safepoint");
      assert((!isa<PHINode>(base) || !cast<PHINode>(base)->getMetadata("is_relocation_phi")) &&
          "If it is a relocation phi, it should be found at the very first joint node");
      return base;
    } else {
      // This node is not reachable from any current statepoint.  We know this
      // since any such statepoint would be responsible for inserting a
      // relocation phi here and we didn't find one.  Since base clearly can
      // reach here, we know there can be no relocations between it and us.
      return base;
    }
  }
  
  struct name_ordering {
    Value* base;
    Value* derived;
    bool operator()(name_ordering const &a, name_ordering const &b) {
      return -1 == a.derived->getName().compare(b.derived->getName());
    }
    
  };
  void stablize_order(std::vector<Value*>& basevec, std::vector<Value*>& livevec) {
    assert( basevec.size() == livevec.size() );

    vector<name_ordering> temp;
    for(size_t i = 0; i < basevec.size(); i++) {
      name_ordering v;
      v.base = basevec[i];
      v.derived = livevec[i];
      temp.push_back(v);
    }
    std::sort(temp.begin(), temp.end(), name_ordering());
    for(size_t i = 0; i < basevec.size(); i++) {
      basevec[i] = temp[i].base;
      livevec[i] = temp[i].derived;
    }
  }

  bool order_by_name(llvm::Value* a, llvm::Value* b) {
    if( a->hasName() && b->hasName() ) {
      return -1 == a->getName().compare(b->getName());
    } else if( a->hasName() && !b->hasName() ) {
      return true;
    } else if( !a->hasName() && b->hasName() ) {
      return false;
    } else {
      // Better than nothing, but not stable
      return a < b;
    }
  }
}
void SafepointPlacementImpl::InsertSafepoint(DominatorTree& DT, const CallSite& CS, CallInst* vm_state) {
  Instruction* inst = CS.getInstruction();

  // A) Identify all gc pointers which are staticly live at the given call
  // site.  Note that the definition of liveness used here interacts in
  // interesting ways with the relocation logic (due to induction assumptions
  // around inserting multiple safepoints.)
  BasicBlock* BB = inst->getParent();
  std::set<llvm::Value*> liveset;
  findLiveGCValuesAtInst(inst, BB, DT, NULL, liveset);

  if( PrintLiveSet ) {
    // Note: This output is used by several of the test cases
    // The order of elemtns in a set is not stable, put them in a vec and sort by name
    std::vector<Value*> temp;
    temp.insert( temp.end(), liveset.begin(), liveset.end() );
    std::sort( temp.begin(), temp.end(), order_by_name );
    errs() << "Live Variables:\n";
    for(std::vector<Value*>::iterator itr = temp.begin(), E = temp.end();
        itr != E; itr++) {
      errs() << " " << (*itr)->getName() << "\n";
      //(*itr)->dump();
    }
  }
  if( PrintLiveSetSize ) {
    errs() << "Safepoint For: " << CS.getCalledValue()->getName() << "\n";
    errs() << "Number live values: " << liveset.size() << "\n";
  }

  // B) Find the base pointers for each live pointer
  std::map<llvm::Value*, llvm::Value*> base_pairs;
  std::set<llvm::Value*> newInsertedDefs;
  findBasePointers(liveset, base_pairs, &DT, newInsertedDefs);
  
  if (PrintBasePointers) {
    errs() << "Base Pairs (w/o Relocation):\n";
    for (std::map<Value *, Value *>::const_iterator I = base_pairs.begin(), E = base_pairs.end();
         I != E;
         ++I) {
      errs() << " derived %" << I->first->getName() << " base %" << I->second->getName() << "\n";
    }
  }

  // Update our base pointers so that they're valid with respect to the
  // relocations inserted by any previous safepoint.  Remember, we're *adding*
  // new uses here.  We need to make sure they're respect w.r.t. prexisting
  // relocations.  The logic here depends on both our base pointer
  // implementation and relocation logic.  (i.e. this is arguably somewhat of a hack.)
  for (std::map<Value *, Value *>::const_iterator I = base_pairs.begin(), E = base_pairs.end();
       I != E; ++I) {
    Value* derived = I->first;
    Value* base = I->second;
    Value* relocated = findCurrentRelocationForBase(inst, base);
    if (newInsertedDefs.find(base) != newInsertedDefs.end())
      assert(base == relocated && "All new defs are just inserted at this safepoint and should not have no relocation");
    if( base != relocated ) {
      base_pairs[derived] = relocated;

      // Note: We can't directly check that relocated was in the live set.  It
      // could have been avaiable, but unused.  (i.e. we could be making it
      // live.)  It could also be newly insert base_phi which is available, but
      // was not previously part of the live set
    }
  }

  for (std::set<llvm::Value*>::iterator defIterator = newInsertedDefs.begin(), defEnd = newInsertedDefs.end();
      defIterator != defEnd; defIterator++) {
    Value* newDef = *defIterator;
    for (Value::use_iterator useIterator = newDef->use_begin(), useEnd = newDef->use_end();
        useIterator != useEnd; useIterator++) {
      Instruction* use = cast<Instruction>(*useIterator);

      // Record new defs those are dominating and live at the safepoint (we need to make sure def dominates safepoint since our liveness analysis has this assumption)
      // Use DT to check instruction domination might not be good for compilation time, and we could change to optimal solution if this turn to be a issue
      if (DT.dominates(cast<Instruction>(newDef), inst) && isLiveAtSafepoint(inst, use, *newDef, DT, NULL)) {
        Value* relocated = findCurrentRelocationForBase(inst, newDef);
        assert(newDef == relocated && "All new defs are just inserted at this safepoint and should not have no relocation");
        // Add the live new defs into liveset and base_pairs
        liveset.insert(newDef);
        base_pairs[newDef] = newDef;
      }
    }
  }

  if (PrintBasePointers) {
    errs() << "Base Pairs: (w/Relocation)\n";
    for (std::map<Value *, Value *>::const_iterator I = base_pairs.begin(), E = base_pairs.end();
         I != E;
         ++I) {
      errs() << " derived %" << I->first->getName() << " base %" << I->second->getName() << "\n";
    }
  }

  // Needed even with relocation of base pointers
  addBasesAsLiveValues(liveset, base_pairs);
  
  if( VerifyAllIR ) { verifyFunction(*BB->getParent()); }

  // Third, do the actual placement.
  if( !BaseRewriteOnly ) {
    
    // Convert to vector for efficient cross referencing. 
    std::vector<Value*> basevec, livevec;
    for(std::set<llvm::Value*>::iterator itr = liveset.begin(), E = liveset.end();
        itr != E; itr++) {
      livevec.push_back(*itr);
      
      assert( base_pairs.find(*itr) != base_pairs.end() );
      Value* base = base_pairs[*itr];
      basevec.push_back( base );
    }
    assert( livevec.size() == basevec.size() );

    // To make the output IR slightly more stable (for use in diffs), ensure a
    // fixed order of the values in the safepoint (by sorting the value name).
    // The order is otherwise meaningless.
    stablize_order(basevec, livevec);
    
    std::vector<Instruction*> newDefs;
    std::pair<Instruction*, Instruction*> safepoint =
      CreateSafepoint(CS, vm_state, basevec, livevec, newDefs);
    
    if( VerifyAllIR ) {
      verifyFunction(*BB->getParent());
      // At this point, we've insert the new safepoint node and all of it's
      // base pointers, but we *haven't* yet performed relocation updates.
      // Minus this one statepoint, the statepoint invariants should hold for
      // all values *including* the newly created base pointers
      Instruction* statepoint = safepoint.first;
      assert( isStatepoint(statepoint) );      
      Value* const_1= ConstantInt::get(Type::getInt32Ty(BB->getParent()->getParent()->getContext()), 1);         
      MDNode* md = MDNode::get(BB->getParent()->getParent()->getContext(), const_1);
      statepoint->setMetadata("ignore_clobber", md);    
      verifySafepointIR(*BB->getParent());
      statepoint->setMetadata("ignore_clobber", NULL); //a.k.a. removeMetadata    

    }
    
    // Update all uses to reflect the new definitions.  This will involve
    // rewriting fairly large chunks of the graph to insert PHIs and fixup uses.
    assert( newDefs.size() == livevec.size() );
    for(unsigned i = 0; i < newDefs.size(); i++) {
      insertPHIsForNewDef(DT, livevec[i], newDefs[i], safepoint);
    }
    
    if( VerifyAllIR ) { verifyFunction(*BB->getParent()); }
    // Note: The fact that we've inserted to the underlying instruction list
    // does not invalidate any iterators since llvm uses a doubly linked list
    // implementation internally.
  }
}




/// ---------------------------------------------
/// Everything below here is the implementation of the various phases of
/// safepoint placement.  The code should be roughly organized by phase with a
/// detailed comment describing the high level algorithm.

bool SafepointPlacementImpl::isLiveAtSafepoint(Instruction *term, Instruction *use, Value& def,
                                               DominatorTree& DT, LoopInfo *LI) {
  // The use of the custom definition of reachability is important for two
  // cases:
  // 1) uses in phis where only some edges are coming from reachable blocks
  // 2) uses which are only reachable by passing through the definition
  // This is in effect, a poor implementation of a liveness analysis and should
  // just be reimplemented as such
  return isPotentiallyReachableNotViaDef(term, use, &def, &DT, LI);
}

/// Conservatively identifies any definitions which might be live at the
/// given instruction.
void SafepointPlacementImpl::findLiveGCValuesAtInst(Instruction* term, BasicBlock* pred, DominatorTree& DT, LoopInfo* LI, std::set<llvm::Value*>& liveValues) {
  liveValues.clear();
  
  assert( isa<CallInst>(term) || isa<InvokeInst>(term) || term->isTerminator() );
  
  Function* F = pred->getParent();
  
  // Are there any gc pointer arguments live over this point?  This needs to be
  // special cased since arguments aren't defined in basic blocks.
  for(Function::arg_iterator argitr = F->arg_begin(), argend = F->arg_end();
      argitr != argend; argitr++) {
    Argument& arg = *argitr;

    if( !isGCPointerType(arg.getType()) ) {
      continue;
    }
     
    for (Value::use_iterator I = arg.use_begin(), E = arg.use_end();
         I != E; I++) {
      Instruction* use = cast<Instruction>(*I);
      if( isLiveAtSafepoint(term, use, arg, DT, LI) ) {
        liveValues.insert(&arg);
        break;
      }
    }
  }
  
  // Walk through all dominating blocks - the ones which can contain
  // definitions used in this block - and check to see if any of the values
  // they define are used in locations potentially reachable from the
  // interesting instruction.  
  for(Function::iterator bbi = F->begin(), bbie = F->end(); bbi != bbie; bbi++){
    BasicBlock *BBI = bbi;
    if(DT.dominates(BBI, pred)){
      if (TraceLSP) {
        errs() << "[LSP] Looking at dominating block " << pred->getName() << "\n";
      }
      BUGPOINT_CLEAN_EXIT_IF( !isPotentiallyReachable(BBI, pred) );
      assert( isPotentiallyReachable(BBI, pred) && "dominated block must be reachable");
      // Walk through the instructions in dominating blocks and keep any
      // that have a use potentially reachable from the block we're
      // considering putting the safepoint in
      for(BasicBlock::iterator itr = BBI->begin(), end = BBI->end();
          itr != end; itr++) {
        Instruction& inst = *itr;

        if (TraceLSP) {
          errs() << "[LSP] Looking at instruction ";
          inst.dump();
        }

        if( pred == BBI && (&inst) == term ) {
          if (TraceLSP) {
            errs() << "[LSP] stopped because we encountered the safepoint instruction.\n";
          }

          // If we're in the block which defines the interesting instruction,
          // we don't want to include any values as live which are defined
          // _after_ the interesting line or as part of the line itself
          // i.e. "term" is the call instruction for a call safepoint, the
          // results of the call should not be considered live in that stackmap
          break;
        }
        
        if( !isGCPointerType(inst.getType()) ) {
          if (TraceLSP) {
            errs() << "[LSP] not considering because inst not of gc pointer type\n";
          }
          continue;
        }
        
        for (Value::use_iterator I = inst.use_begin(), E = inst.use_end();
             I != E; I++) {
          Instruction* use = cast<Instruction>(*I);
          if (isLiveAtSafepoint(term, use, inst, DT, LI)) {
            if (TraceLSP) {
              errs() << "[LSP] found live use for this safepoint ";
              use->dump();
            }
            liveValues.insert(&inst);
            break;
          } else {
            if (TraceLSP) {
              errs() << "[LSP] this use does not satisfy isLiveAtSafepoint ";
              use->dump();
            }
          }
        }
      }
    }
  }
}

namespace {
  /// Helper function for findBasePointer - Will return a value which either a)
  /// defines the base pointer for the input or b) blocks the simple search
  /// (i.e. a PHI or Select of two derived pointers)
  Value* findBaseDefiningValue(Value* I) {
    assert( I->getType()->isPointerTy() && "Illegal to ask for the base pointer of a non-pointer type");

    // There are instructions which can never return gc pointer values.  Sanity check
    // that this is actually true.
    assert( !isa<InsertElementInst>(I) && !isa<ExtractElementInst>(I) &&
            !isa<ShuffleVectorInst>(I) && "Vector types are not gc pointers");
    assert( (!isa<Instruction>(I) || isa<InvokeInst>(I) || !cast<Instruction>(I)->isTerminator()) &&
            "With the exception of invoke terminators don't define values");
    assert( !isa<StoreInst>(I) && !isa<FenceInst>(I) && "Can't be definitions to start with");
    assert( !isa<ICmpInst>(I) && !isa<FCmpInst>(I) && "Comparisons don't give ops");
    // There's a bunch of instructions which just don't make sense to apply to
    // a pointer.  The only valid reason for this would be pointer bit
    // twiddling which we're just not going to support.
    assert( (!isa<Instruction>(I) || !cast<Instruction>(I)->isBinaryOp()) &&
            "Binary ops on pointer values are meaningless.  Unless your bit-twiddling which we don't support");
        
    if( Argument *Arg = dyn_cast<Argument>(I) ) {
      // An incoming argument to the function is a base pointer
      // We should have never reached here if this argument isn't an gc value
      assert( Arg->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      return Arg;
    }

    if( GlobalVariable* global =  dyn_cast<GlobalVariable>(I) ) {
      // base case
      assert( AllFunctions && "should not encounter a global variable as an gc base pointer in the VM");
      assert( global->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      return global;
    }

    if( UndefValue* undef = dyn_cast<UndefValue>(I) ) {
      // This case arises when the optimizer has recognized undefined
      // behavior.  It's also really really common when using bugpoint to
      // reduce failing cases.
      BUGPOINT_CLEAN_EXIT_IF( !AllFunctions );
      assert( AllFunctions && "should not encounter a undef base in the VM");
      assert( undef->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      return undef; //utterly meaningless, but useful for dealing with
                    //partially optimized code.
    }

    // Due to inheritance, this must be _after_ the global variable and undef checks
    if( Constant* con = dyn_cast<Constant>(I) ) {
      assert( !isa<GlobalVariable>(I) && !isa<UndefValue>(I) && "order of checks wrong!");
      // Note: Finding a constant base for something marked for relocation
      // doesn't really make sense.  The most likely case is either a) some
      // screwed up the address space usage or b) your validating against
      // compiled C++ code w/o the proper separation.  The only real exception
      // is a null pointer.  You could have generic code written to index of
      // off a potentially null value and have proven it null.  We also use
      // null pointers in dead paths of relocation phis (which we might later
      // want to find a base pointer for).
      assert( con->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      assert( con->isNullValue() && "null is the only case which makes sense");
      return con;
    }
    
    if( CastInst *CI = dyn_cast<CastInst>(I) ) {
      Value* def = CI->stripPointerCasts();
      assert( def->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      if( isa<CastInst>(def) ) {
        // If we find a cast instruction here, it means we've found a cast
        // which is not simply a pointer cast (i.e. an inttoptr).  We don't
        // know how to handle int->ptr conversion in general, but we need to
        // handle a few special cases before failing.  
        IntToPtrInst* i2p = cast<IntToPtrInst>(def);
        // If the frontend marked this as a known base pointer...
        if ( i2p->getMetadata("verifier_exception") ) { return def; }

        // For validating against C++ hand written examples, we're just
        // going to pretend that this is a base pointer in it's own right.
        // It's a purely manufactored pointer.  This is not safe in general,
        // but is fine for manually written test cases.
        if( AllFunctions ) {
          errs() << "warning: treating int as fake base: "; def->dump();
          return def;
        }
        // Fail hard on the general case.
        llvm_unreachable("Can not find the base pointers for an inttoptr cast");
      }
      assert( !isa<CastInst>(def) && "shouldn't find another cast here");
      return findBaseDefiningValue( def );
    }
    
    if(LoadInst* LI = dyn_cast<LoadInst>(I)) {
      if( LI->getType()->isPointerTy() ) {
        Value *Op = LI->getOperand(0);
        // Has to be a pointer to an gc object, or possibly an array of such?
        assert( Op->getType()->isPointerTy() );
        return LI; //The value loaded is an gc base itself
      }
    }
    if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(I)) {
      Value *Op = GEP->getOperand(0);
      if( Op->getType()->isPointerTy() ) {
        return findBaseDefiningValue(Op); // The base of this GEP is the base
      }
    }

    if( AllocaInst* alloc = dyn_cast<AllocaInst>(I) ) {
      // An alloca represents a conceptual stack slot.  It's the slot itself
      // that the GC needs to know about, not the value in the slot.
      assert( alloc->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      assert( AllFunctions && "should not encounter a alloca as an gc base pointer in the VM");
      return alloc;
    }

    if( IntrinsicInst *II = dyn_cast<IntrinsicInst>(I) ) {
      switch(II->getIntrinsicID()) {
      default:
        // fall through to general call handling
        break;
      case Intrinsic::gc_relocate: {
        // A previous relocation is a base case.  Examine the statepoint nodes
        // to find the gc_relocate which relocates the base of the pointer
        // we're interested in.  
        CallInst* statepoint = cast<CallInst>(II->getArgOperand(0));
        ConstantInt* base_offset = cast<ConstantInt>(II->getArgOperand(1));
        for (Value::use_iterator I = statepoint->use_begin(), E = statepoint->use_end();
             I != E; I++) {
          IntrinsicInst* use = dyn_cast<IntrinsicInst>(*I);
          // can be a gc_result use as well, we should ignore that
          if( use && use->getIntrinsicID() == Intrinsic::gc_relocate ) {
            ConstantInt* relocated = cast<ConstantInt>(use->getArgOperand(2));
            if( relocated == base_offset ) {
              // Note: use may be II if this was a base relocation.
              return use;
            }
          }
        }
        llvm_unreachable("must have relocated the base pointer");
      }
      case Intrinsic::statepoint:
        assert( false &&
                "We should never reach a statepoint in the def-use chain."
                "  That would require we're tracking the token returned.");
        break;
      case Intrinsic::gcroot:
        // Currently, this mechanism hasn't been extended to work with gcroot.
        // There's no reason it couldn't be, but I haven't thought about the
        // implications much.
        assert( false && "Not yet handled");
      }
    }
    // Let's assume that any call we see is to a java function.  Java
    // functions can only return Java objects (i.e. base pointers).
    // Note: when we add runtime functions which return non-base pointers we
    // will need to revisit this.  (Will this ever happen?)
    if( CallInst* call = dyn_cast<CallInst>(I) ) {
      assert( call->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      return call;
    }
    if( InvokeInst* invoke = dyn_cast<InvokeInst>(I) ) {
      assert( invoke->getType()->isPointerTy() && "Base for pointer must be another pointer" );
      return invoke;
    }

    // I have absolutely no idea how to implement this part yet.  It's not
    // neccessarily hard, I just haven't really looked at it yet.
    assert( !isa<LandingPadInst>(I) && "Landing Pad is unimplemented");

    if( AtomicCmpXchgInst* cas = dyn_cast<AtomicCmpXchgInst>(I) ) {
      // A CAS is effectively a atomic store and load combined under a
      // predicate.  From the perspective of base pointers, we just treat it
      // like a load.  We loaded a pointer from a address in memory, that value
      // had better be a valid base pointer.
      return cas->getPointerOperand();
    }
    if( AtomicRMWInst* atomic = dyn_cast<AtomicRMWInst>(I) ) {
      assert( AtomicRMWInst::Xchg == atomic->getOperation() && "All others are binary ops which don't apply to base pointers");
      // semantically, a load, store pair.  Treat it the same as a standard load
      return atomic->getPointerOperand();
    }


    // The aggregate ops.  Aggregates can either be in the heap or on the
    // stack, but in either case, this is simply a field load.  As a result,
    // this is a defining definition of the base just like a load is.
    if( ExtractValueInst* ev = dyn_cast<ExtractValueInst>(I) ) {
      return ev;
    }

    // We should never see an insert vector since that would require we be 
    // tracing back a struct value not a pointer value.
    assert( !isa<InsertValueInst>(I) && "Base pointer for a struct is meaningless");

    // The last two cases here don't return a base pointer.  Instead, they
    // return a value which dynamically selects from amoung several base
    // derived pointers (each with it's own base potentially).  It's the job of
    // the caller to resolve these.
    if( SelectInst* select = dyn_cast<SelectInst>(I) ) {
      return select;
    }
    if( PHINode* phi = dyn_cast<PHINode>(I) ) {
      return phi;
    }

    errs() << "unknown type: ";
    I->dump();
    assert( false && "unknown type");
    return NULL;
  }

  typedef std::map<Value*, Value*> DefiningValueMapTy;
  Value* findBaseDefiningValueCached(Value* I, DefiningValueMapTy& cache) {
    if (cache.find(I) == cache.end()) {
      cache[I] = findBaseDefiningValue(I);
    }
    assert(cache.find(I) != cache.end());

    if( TraceLSP ) {
      errs() << "fBDV-cached: " << I->getName() << " -> " << cache[I]->getName() << "\n";
    }
    return cache[I];
  }


  //TODO: find a better name for this
  class PhiState {
   public:
    enum Status {
      Unknown,
      Base,
      Conflict
    };

    PhiState(Status s, Value* b = NULL) : status(s), base(b) {
      assert(status != Base || b);
    }
    PhiState(Value* b) : status(Base), base(b) {}
    PhiState() : status(Unknown), base(NULL) {}
    PhiState(const PhiState &other) : status(other.status), base(other.base) {
      assert(status != Base || base);
    }

    Status getStatus() const { return status; }
    Value *getBase() const { return base; }

    bool isBase() const { return getStatus() == Base; }
    bool isUnknown() const { return getStatus() == Unknown; }
    bool isConflict() const { return getStatus() == Conflict; }

    bool operator==(const PhiState &other) const {
      return base == other.base && status == other.status;
    }

    bool operator!=(const PhiState &other) const {
      return !(*this == other);
    }

    void dump() {
      errs() << status << " (" << base << " - " << (base ? base->getName() : "NULL") << "): ";
    }

   private:
    Status status;
    Value* base; //non null only if status == base
  };

  // Values of type PhiState form a lattice, and this is a helper
  // class that implementes the meet operation.  The meat of the meet
  // operation is implemented in MeetPhiStates::pureMeet
  class MeetPhiStates {
   public:
    // phiStates is a mapping from PHINodes and SelectInst's to PhiStates.
    explicit MeetPhiStates(const std::map<Value *, PhiState> &phiStates) :
        phiStates(phiStates) { }

    // Destructively meet the current result with the base V.  V can
    // either be a merge instruction (SelectInst / PHINode), in which
    // case its status is looked up in the phiStates map; or a regular
    // SSA value, in which case it is assumed to be a base.
    void meetWith(Value *V) {
      PhiState otherState = getStateForBase(V);
      assert((MeetPhiStates::pureMeet(otherState, currentResult) ==
              MeetPhiStates::pureMeet(currentResult, otherState)) &&
             "math is wrong: meet does not commute!");
      currentResult = MeetPhiStates::pureMeet(otherState, currentResult);
    }

    PhiState getResult() const { return currentResult; }

   private:
    const std::map<Value *, PhiState> &phiStates;
    PhiState currentResult;

    PhiState getStateForBase(Value *baseValue) {
      if (SelectInst *SI = dyn_cast<SelectInst>(baseValue)) {
        return lookupFromMap(SI);
      } else if (PHINode *PN = dyn_cast<PHINode>(baseValue)) {
        return lookupFromMap(PN);
      } else {
        return PhiState(baseValue);
      }
    }

    PhiState lookupFromMap(Value *V) {
      map<Value *, PhiState>::const_iterator I = phiStates.find(V);
      assert(I != phiStates.end() && "lookup failed!");
      return I->second;
    }

    static PhiState pureMeet(const PhiState &stateA, const PhiState &stateB) {
      switch (stateA.getStatus()) {
        default:
          llvm_unreachable("extra state found?");
        case PhiState::Unknown:
          return stateB;

        case PhiState::Base:
          assert( stateA.getBase() && "can't be null");
          if (stateB.isUnknown()) {
            return stateA;
          } else if (stateB.isBase()) {
            if (stateA.getBase() == stateB.getBase()) {
              assert(stateA == stateB && "equality broken!");
              return stateA;
            }
            return PhiState(PhiState::Conflict);
          } else {
            assert(stateB.isConflict() && "only three states!");
            return PhiState(PhiState::Conflict);
          }

        case PhiState::Conflict:
          return stateA;
      }
      assert(false && "only three states!");
    }
  };

  /// For a given value or instruction, figure out what base ptr it's derived
  /// from.  For gc objects, this is simply itself.  On success, returns a value
  /// which is the base pointer.  (This is reliable and can be used for
  /// relocation.)  On failure, returns NULL.
  Value* findBasePointer(Value* I, DefiningValueMapTy& cache, std::set<llvm::Value*>& newInsertedDefs) {
    Value* def =  findBaseDefiningValueCached(I, cache);

    if (!isa<PHINode>(def) && !isa<SelectInst>(def)) {
      return def;
    }

    // If we encounter something which we know is a base value from a previous
    // iteration of safepoint insertion, we can just use that.
    if( EnableKnownBaseOpt && cast<Instruction>(def)->getMetadata("is_base_value") ) {
      return def;
    }
    // As an extension of the above check, in theory, you could leverage
    // any previous existing subgraph of base values.  This is an optimization
    // and should NOT be done until fully stable.



    /* Here's the rough algorithm:
       - For every SSA value, construct a mapping to either an actual base
       pointer or a PHI which obscures the base pointer.
       - Construct a mapping from PHI to unknown TOP state.  Use an
       optimistic algorithm to propagate base pointer information.  Lattice
       looks like:
       UNKNOWN
       b1 b2 b3 b4
       CONFLICT
       When algorithm terminates, all PHIs will either have a single concrete
       base or be in a conflict state.
       - For every conflict, insert a dummy PHI node without arguments.  Add
       these to the base[Instruction] = BasePtr mapping.  For every
       non-conflict, add the actual base.
       - For every conflict, add arguments for the base[a] of each input
       arguments.

       Note: A simpler form of this would be to add the conflict form of all
       PHIs without running the optimistic algorithm.  This would be
       analougous to pessimistic data flow and would likely lead to an
       overall worse solution.
    */

    map<Value*, PhiState> states;
    states[def] = PhiState();
    // Recursively fill in all phis & selects reachable from the initial one
    // PERF: Yes, this is as horribly inefficient as it looks.
    bool done = false;
    while (!done) {
      done = true;
      for (map<Value*, PhiState>::iterator itr = states.begin(), end = states.end();
          itr != end;
          itr++) {
        Value* v = itr->first;
        if (PHINode* phi = dyn_cast<PHINode>(v)) {
          unsigned NumPHIValues = phi->getNumIncomingValues();
          assert( NumPHIValues > 0 && "zero input phis are illegal");
          for (unsigned i = 0; i != NumPHIValues; ++i) {
            Value *InVal = phi->getIncomingValue(i);
            Value *local = findBaseDefiningValueCached(InVal, cache);
            if (states.find(local) == states.end() &&
                (isa<PHINode>(local) || isa<SelectInst>(local))) {
              states[local] = PhiState();
              done = false;
            }
          }
        } else if (SelectInst* sel = dyn_cast<SelectInst>(v)) {
          Value* local = findBaseDefiningValueCached(sel->getTrueValue(), cache);
          if (states.find(local) == states.end() &&
              (isa<PHINode>(local) || isa<SelectInst>(local))) {
            states[local] = PhiState();
            done = false;
          }
          local = findBaseDefiningValueCached(sel->getFalseValue(), cache);
          if (states.find(local) == states.end() &&
              (isa<PHINode>(local) || isa<SelectInst>(local))) {
            states[local] = PhiState();
            done = false;
          }
        }
      }
    }

    if( TraceLSP ) {
      errs() << "States after initialization:\n";
      for (map<Value*, PhiState>::iterator itr = states.begin(), end = states.end();
           itr != end;
           itr++) {
        Instruction* v = cast<Instruction>(itr->first);
        PhiState state = itr->second;
        state.dump();
        v->dump();
      }
    }

    

    // TODO: come back and revisit the state transitions around inputs which
    // have reached conflict state.  The current version seems too conservative.

    bool progress = true;
    size_t oldSize = 0;
    while (progress) {
      oldSize = states.size();
      progress = false;
      for (map<Value*, PhiState>::iterator itr = states.begin(), end = states.end();
           itr != end;
           itr++) {
        MeetPhiStates calculateMeet(states);
        Value *v = itr->first;

        assert(isa<SelectInst>(v) || isa<PHINode>(v));
        if (SelectInst *select = dyn_cast<SelectInst>(v)) {
          calculateMeet.meetWith(findBaseDefiningValueCached(select->getTrueValue(), cache));
          calculateMeet.meetWith(findBaseDefiningValueCached(select->getFalseValue(), cache));
        } else if (PHINode *phi = dyn_cast<PHINode>(v)) {
          for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
            calculateMeet.meetWith(findBaseDefiningValueCached(phi->getIncomingValue(i), cache));
          }
        } else {
          llvm_unreachable("no such state expected");
        }

        PhiState oldState = states[v];
        PhiState newState = calculateMeet.getResult();
        if (oldState != newState) {
          progress = true;
          states[v] = newState;
        }
      }

      assert(oldSize <= states.size());
      assert(oldSize == states.size() || progress);
    }

    if( TraceLSP ) {
      errs() << "States after meet iteration:\n";
      for (map<Value*, PhiState>::iterator itr = states.begin(), end = states.end();
           itr != end;
           itr++) {
        Instruction* v = cast<Instruction>(itr->first);
        PhiState state = itr->second;
        state.dump();
        v->dump();
      }
    }

    // Insert Phis for all conflicts
    for (map<Value*, PhiState>::iterator itr = states.begin(), end = states.end();
         itr != end;
         itr++) {
      Instruction* v = cast<Instruction>(itr->first);
      PhiState state = itr->second;

      assert(!state.isUnknown() && "Optimistic algorithm didn't complete!");
      if (state.isConflict()) {
        if (isa<PHINode>(v)) {
          int num_preds = std::distance(pred_begin(v->getParent()), pred_end(v->getParent()));
          assert( num_preds > 0 && "how did we reach here" );
          PHINode* phi = PHINode::Create(v->getType(), num_preds, "base_phi", v);
          newInsertedDefs.insert(phi);
          // Add metadata marking this as a base value
          Value* const_1= ConstantInt::get(Type::getInt32Ty(v->getParent()->getParent()->getParent()->getContext()), 1);         
          MDNode* md = MDNode::get(v->getParent()->getParent()->getParent()->getContext(), const_1);
          phi->setMetadata("is_base_value", md);
          states[v] = PhiState(PhiState::Conflict, phi);
        } else if (SelectInst* sel = dyn_cast<SelectInst>(v)) {
          // The undef will be replaced later
          UndefValue* undef = UndefValue::get(sel->getType());
          SelectInst* basesel = SelectInst::Create(sel->getCondition(), undef, undef, "base_select", sel);
          newInsertedDefs.insert(basesel);
          // Add metadata marking this as a base value
          Value* const_1= ConstantInt::get(Type::getInt32Ty(v->getParent()->getParent()->getParent()->getContext()), 1);         
          MDNode* md = MDNode::get(v->getParent()->getParent()->getParent()->getContext(), const_1);
          basesel->setMetadata("is_base_value", md);
          states[v] = PhiState(PhiState::Conflict, basesel);
        } else {
          assert(false);
        }
      }
    }

    // Fixup all the inputs of the new PHIs
    for (map<Value*, PhiState>::iterator itr = states.begin(), end = states.end();
         itr != end;
         itr++) {
      Instruction* v = cast<Instruction>(itr->first);
      PhiState state = itr->second;

      assert(!state.isUnknown() && "Optimistic algorithm didn't complete!");
      if (state.isConflict()) {
        if (PHINode* basephi = dyn_cast<PHINode>(state.getBase())) {
          PHINode* phi = cast<PHINode>(v);
          unsigned NumPHIValues = phi->getNumIncomingValues();
          for (unsigned i = 0; i < NumPHIValues; i++) {
            Value *InVal = phi->getIncomingValue(i);
            BasicBlock* InBB = phi->getIncomingBlock(i);
            // Find either the defining value for the PHI or the normal base for
            // a non-phi node
            Value* base = findBaseDefiningValueCached(InVal, cache);
            if (isa<PHINode>(base) || isa<SelectInst>(base)) {
              // Either conflict or base.
              base = states[base].getBase();
              assert(base != NULL && "unknown PhiState!");
            }
            assert(base && "can't be null");
            // Must use original input BB since base may not be Instruction
            // The cast is needed since base traversal may strip away bitcasts
            if (base->getType() != basephi->getType()) {
              base = new BitCastInst(base, basephi->getType(), "cast", InBB->getTerminator());
            }
            basephi->addIncoming(base, InBB);
          }
          assert(basephi->getNumIncomingValues() == NumPHIValues);
        } else if (SelectInst* basesel = dyn_cast<SelectInst>(state.getBase())) {
          SelectInst* sel = cast<SelectInst>(v);
          // Operand 1 & 2 are true, false path respectively. TODO: refactor to
          // something more safe and less hacky.
          for (int i = 1; i <= 2; i++) {
            Value *InVal = sel->getOperand(i);
            // Find either the defining value for the PHI or the normal base for
            // a non-phi node
            Value* base = findBaseDefiningValueCached(InVal, cache);
            if (isa<PHINode>(base) || isa<SelectInst>(base)) {
              // Either conflict or base.
              base = states[base].getBase();
              assert(base != NULL && "unknown PhiState!");
            }
            assert(base && "can't be null");
            // Must use original input BB since base may not be Instruction
            // The cast is needed since base traversal may strip away bitcasts
            if (base->getType() != basesel->getType()) {
              base = new BitCastInst(base, basesel->getType(), "cast", basesel);
            }
            basesel->setOperand(i, base);
          }
        } else {
          assert(false && "unexpected type");
        }
      }
    }

    return states[def].getBase();
  }
}
/// For a set of live pointers (base and/or derived), identify the base
/// pointer of the object which they are derived from.
void SafepointPlacementImpl::findBasePointers(
    const std::set<llvm::Value*>& live,
    std::map<llvm::Value*, llvm::Value*>& base_pairs,
    DominatorTree *DT, std::set<llvm::Value*>& newInsertedDefs) {

  // PERF: We currently cache the queuries accross all live values in a given
  // safepoint.  It might be reorg-ing the code to share this map across
  // safepoints.  Probably not worth it for the moment since the dominators
  // are the key performance inhibitor.  Where it might help is in needing
  // fewer invalidations.
  /* scope */ {
    DefiningValueMapTy cache;
    for(std::set<llvm::Value*>::iterator I = live.begin(), E = live.end();
        I != E; I++) {
      Value *ptr = *I;
      Value *base = findBasePointer(ptr, cache, newInsertedDefs);
      assert( base && "failed to find base pointer");
      base_pairs[ptr] = base;
      assert(!isa<Instruction>(base) ||
             !isa<Instruction>(ptr) ||
             DT->dominates(cast<Instruction>(base)->getParent(),
                           cast<Instruction>(ptr)->getParent()) &&
             "The base we found better dominate the derived pointer");
    }
  }
}

CallInst* SafepointPlacementImpl::findVMState(llvm::Instruction* term, DominatorTree *DT) {
  // At this time, we look for a vmstate call dominating 'term'
  // By construction, if there was one in the original IR generated by the
  // frontend, a valid one is still available.  

  BasicBlock::reverse_iterator I = term->getParent()->rbegin(), E = term->getParent()->rend();
  // Find the BasicBlock::reverse_iterator pointing to term.  Is there a direct way to do this?
  while (&*I != term && I != E) I++;
  assert(I != E && "term not in it's own BasicBlock?!");

  while (true) {
    // We search [I, E) for a VM state instruction.
    BasicBlock::reverse_iterator maybeVMS = I;
    while (maybeVMS != E) {
      if (isJVMState(&*maybeVMS)) {
        return cast<CallInst>(&*maybeVMS);
      }
      maybeVMS++;
    }

    // We couldn't find a VM state in the current BasicBlock, go to its
    // immediate dominator.
    BUGPOINT_CLEAN_EXIT_IF(!I->getParent());
    BUGPOINT_CLEAN_EXIT_IF(!DT->getNode(I->getParent()));
    BUGPOINT_CLEAN_EXIT_IF(!DT->getNode(I->getParent())->getIDom());
    if( !DT->getNode(I->getParent())->getIDom() ) break; // and crash
    BasicBlock *immediateDominator = DT->getNode(I->getParent())->getIDom()->getBlock();
    BUGPOINT_CLEAN_EXIT_IF(!immediateDominator);
    if (immediateDominator == NULL) break; // and crash!

    I = immediateDominator->rbegin();
    E = immediateDominator->rend();
  }

  return NULL;
}

namespace {

  void VerifySafepointBounds(const std::pair<Instruction*, Instruction*>& bounds) {
    assert( bounds.first->getParent() && bounds.second->getParent() &&
            "both must belong to basic blocks");
    if( bounds.first->getParent() == bounds.second->getParent() ) {
      // This is a call safepoint
      // TODO: scan the range to find the statepoint
      // TODO: check that the following instruction is not a gc_relocate or gc_result
    } else {
      // This is an invoke safepoint
      BasicBlock* BB = bounds.first->getParent();
      InvokeInst* invoke = dyn_cast<InvokeInst>(BB->getTerminator());
      assert( invoke && "only continues over invokes!");
      assert( invoke->getNormalDest() == bounds.second->getParent() &&
              "safepoint can only continue into normal exit block");
    }
  }
  
  int find_index(const std::vector<Value*>& livevec, Value* val) {
    std::vector<Value*>::const_iterator itr = std::find(livevec.begin(), livevec.end(), val);
    assert( livevec.end() != itr );
    size_t index = std::distance(livevec.begin(), itr);
    assert( index >= 0 && index < livevec.size() );
    return index;
  }
}

/// Inserts the actual code for a safepoint.  WARNING: Does not do any fixup
/// to adjust users of the original live values.  That's the callers responsibility.
std::pair<Instruction*, Instruction*>
SafepointPlacementImpl::CreateSafepoint(const CallSite& CS, /* to replace */
                                        llvm::CallInst* jvmStateCall,
                                        const std::vector<llvm::Value*>& basePtrs,
                                        const std::vector<llvm::Value*>& liveVariables,
                                        std::vector<llvm::Instruction*>& newDefs) {
  assert( basePtrs.size() == liveVariables.size() );
  
  BasicBlock* BB = CS.getInstruction()->getParent();
  assert(BB);
  Function* F = BB->getParent();
  assert(F && "must be set");
  Module* M = F->getParent();
  assert(M && "must be set");

  // TODO: technically, a pass is not allowed to get functions from within a
  // function pass since it might trigger a new function addition.  Refactor
  // this logic out to the initialization of the pass.  Doesn't appear to
  // matter in practice.

  // Fill in the one generic type'd argument (the function is also vararg)
  std::vector<Type*> argTypes;
  argTypes.push_back( CS.getCalledValue()->getType() );
  Function* gc_statepoint_decl =
    Intrinsic::getDeclaration(M, Intrinsic::statepoint, argTypes);

  // Then go ahead and use the builder do actually do the inserts.  We insert
  // immediately before the previous instruction under the assumption that all
  // arguments will be available here.  We can't insert afterwards since we may
  // be replacing a terminator.
  Instruction* insertBefore = CS.getInstruction();
  IRBuilder<> Builder(insertBefore);
  // First, create the statepoint (with all live ptrs as arguments).
  std::vector<llvm::Value*> args;
  // target, #args, flags, bci, #stack, #locals, #monitors
  Value* Target = CS.getCalledValue();
  args.push_back( Target );
  args.push_back( ConstantInt::get( Type::getInt32Ty(M->getContext()), CS.arg_size()) ); 
  args.push_back( ConstantInt::get( Type::getInt32Ty(M->getContext()), 0 /*unused*/) );

  IntegerType *i32Ty = Type::getInt32Ty(M->getContext());

  if( jvmStateCall ) {
    // Bugpoint doesn't know these are special and tries to remove arguments
    BUGPOINT_CLEAN_EXIT_IF(jvmStateCall->getNumArgOperands() < 4);

    JVMState jvmState(jvmStateCall);

    args.push_back( ConstantInt::get( i32Ty, jvmState.bci() ) );
    args.push_back( ConstantInt::get( i32Ty, jvmState.numStackElements() ) );
    args.push_back( ConstantInt::get( i32Ty, jvmState.numLocals() ) );
    args.push_back( ConstantInt::get( i32Ty, jvmState.numMonitors() ) );
  } else {
    // All of these are placeholders until we actually grab the deopt state
    args.push_back( ConstantInt::get( i32Ty, -1) );
    args.push_back( ConstantInt::get( i32Ty, 0) );
    args.push_back( ConstantInt::get( i32Ty, 0) );
    args.push_back( ConstantInt::get( i32Ty, 0) );
  }


  // Copy all the arguments of the original call
  args.insert( args.end(), CS.arg_begin(), CS.arg_end());

  if( jvmStateCall ) {
    JVMState jvmState(jvmStateCall);

    for (int i = 0; i < jvmState.numStackElements(); i++) {
      args.push_back(jvmState.stackElementAt(i));
    }

    for (int i = 0; i < jvmState.numLocals(); i++) {
      args.push_back(jvmState.localAt(i));
    }

    for (int i = 0; i < jvmState.numMonitors(); i++) {
      args.push_back(jvmState.monitorAt(i));
    }
  }

  // add all the pointers to be relocated (gc arguments)
  // Capture the start of the live variable list for use in the gc_relocates
  const int live_start = args.size();
  args.insert( args.end(), liveVariables.begin(), liveVariables.end() );

  // Create the statepoint given all the arguments
  Instruction* token = NULL;
  if( CS.isCall() ) {
    CallInst* toReplace = cast<CallInst>(CS.getInstruction());
    CallInst* call = Builder.CreateCall(gc_statepoint_decl, args, "safepoint_token");
    call->setTailCall(toReplace->isTailCall());
    call->setCallingConv(toReplace->getCallingConv());
    // I believe this copies both param and function attributes - TODO: test
    call->setAttributes(toReplace->getAttributes());
    token = call;

    // Put the following gc_result and gc_relocate calls immediately after the
    // the old call (which we're about to delete)
    BasicBlock::iterator next(toReplace);
    assert( BB->end() != next && "not a terminator, must have next");
    next++;
    Instruction* IP = &*(next);
    Builder.SetInsertPoint(IP);
    Builder.SetCurrentDebugLocation(IP->getDebugLoc());
    
  } else if( CS.isInvoke() ) {
    InvokeInst* toReplace = cast<InvokeInst>(CS.getInstruction());

    // Insert a new basic block which will become the normal destination of our
    // modified invoke.  This is needed since the original normal destinition
    // can potentially be reachable along other paths.  
    BasicBlock* normalDest = 
      BasicBlock::Create(M->getContext(),
                         "invoke_safepoint_normal_dest",
                         F, toReplace->getNormalDest());
    BranchInst::Create(toReplace->getNormalDest(), normalDest);

    // Loop over any phi nodes in the original normal dest, update them to
    // point to the newly inserted block rather than the invoke BB.  
    /* scope */ {
      PHINode *PN = NULL;
      for (BasicBlock::iterator II = toReplace->getNormalDest()->begin();
           (PN = dyn_cast<PHINode>(II)); ++II) {
        int IDX = PN->getBasicBlockIndex(toReplace->getParent());
        while (IDX != -1) {
          PN->setIncomingBlock((unsigned)IDX, normalDest);
          IDX = PN->getBasicBlockIndex(toReplace->getParent());
        }
      }
    }

    // TODO: since we're inserting basic blocks, do we need to update either DT
    // or LI? Or stop claiming to preserveCFG?

    // Insert the new invoke into the old block.  We'll remove the old one in a
    // moment at which point this will become the new terminator for the
    // original block.
    InvokeInst* invoke = Builder.CreateInvoke(gc_statepoint_decl,
                                              normalDest,
                                              toReplace->getUnwindDest(),
                                              args); 
    invoke->setCallingConv(toReplace->getCallingConv());
    // I believe this copies both param and function attributes - TODO: test
    invoke->setAttributes(toReplace->getAttributes());
    token = invoke;

    // Put all the gc_result and gc_return value calls into the normal control
    // flow block
    Instruction* IP = &*(normalDest->getFirstInsertionPt());
    Builder.SetInsertPoint(IP);
    Builder.SetCurrentDebugLocation(toReplace->getDebugLoc());
  } else {
    llvm_unreachable("unexpect type of CallSite");
  }
  assert(token);

  // Handle the return value of the original call - update all uses to use a
  // gc_result hanging off the statepoint node we just inserted
  
  // Only add the gc_result iff there is actually a used result
  Instruction* gc_result = NULL;
  if( !CS.getType()->isVoidTy() &&
      !CS.getInstruction()->use_empty() ) {
    vector<Type*> types; //one per 'any' type
    types.push_back( CS.getType() ); //result type
    Value* gc_result_func = NULL;
    if( CS.getType()->isIntegerTy() ) {
      gc_result_func = Intrinsic::getDeclaration(M, Intrinsic::gc_result_int, types);
    } else if( CS.getType()->isFloatingPointTy() ) {
      gc_result_func = Intrinsic::getDeclaration(M, Intrinsic::gc_result_float, types);
    } else if( CS.getType()->isPointerTy() ) {
      gc_result_func = Intrinsic::getDeclaration(M, Intrinsic::gc_result_ptr, types);
    } else {
      llvm_unreachable("non java type encountered");
    }
    
    vector<Value*> args;
    args.push_back( token );
    gc_result = Builder.CreateCall(gc_result_func, args, CS.getInstruction()->hasName() ? CS.getInstruction()->getName() : "");

    // Replace all uses with the new call
    CS.getInstruction()->replaceAllUsesWith(gc_result);
  }

  // Now that we've handled all uses, remove the original call itself
  // Note: The insert point can't be the deleted instruction!
  CS.getInstruction()->eraseFromParent();
  /* scope */ {
    // trip an assert if somehow this isn't a terminator
    TerminatorInst* TI = BB->getTerminator();
    (void)TI;
    assert( (CS.isCall() || TI == token) && "newly insert invoke is not terminator?");
  }


  // Second, create a gc.relocate for every live variable
  for(unsigned i = 0; i < liveVariables.size(); i++) {
    // We generate a (potentially) unique declaration for every pointer type
    // combination.  This results is some blow up the function declarations in
    // the IR, but removes the need for argument bitcasts which shrinks the IR
    // greatly and makes it much more readable.
    vector<Type*> types; //one per 'any' type
    types.push_back( liveVariables[i]->getType() ); //result type
    Value* gc_relocate_decl = Intrinsic::getDeclaration(M, Intrinsic::gc_relocate, types);

    // Generate the gc.relocate call and save the result
    vector<Value*> args;
    args.push_back( token );
    args.push_back( ConstantInt::get( Type::getInt32Ty(M->getContext()), live_start + find_index(liveVariables, basePtrs[i])) );
    args.push_back( ConstantInt::get( Type::getInt32Ty(M->getContext()), live_start + find_index(liveVariables, liveVariables[i])) );
    // only specify a debug name if we can give a useful one
    Value* reloc = Builder.CreateCall(gc_relocate_decl, args, liveVariables[i]->hasName() ? liveVariables[i]->getName() + ".relocated" : "");
    // Trick CodeGen into thinking there are lots of free registers at this
    // fake call.  
    cast<CallInst>(reloc)->setCallingConv(CallingConv::Cold);

    newDefs.push_back( cast<Instruction>(reloc) );
  }
  assert( newDefs.size() == liveVariables.size() && "missing or extra redefinition at safepoint");

  // PERF: Using vectors where array literals and reserves would be better.

  // Need to pass through the last part of the safepoint block so that we
  // don't accidentally update uses in a following gc.relocate which is
  // still conceptually part of the same safepoint.  Gah.
  Instruction* last = NULL;
  if( !newDefs.empty() ) {
    last = newDefs.back();
  } else if( gc_result ) {
    last = gc_result;
  } else {
    last = token;
  }
  assert(last && "can't be null");
  const std::pair<Instruction*, Instruction*> bounds =
    make_pair( token, last);

  // Sanity check our results - this is slightly non-trivial due to invokes
  VerifySafepointBounds(bounds);
  
  return bounds;
}

namespace {

  bool isKnownBasePointer(Value* def) {
    // Note: In practice, newDef will always be a gc_relocate in which case
    // this check is nearly trivial.  We could use simpler code here, but this
    // is correct in general and might be useful in the future.
    
    // Strip and bitcasts, zero geps, or aliases (does not effect baseness)
    def = def->stripPointerCasts();

    // If we encounter something which we know is a base value from a previous
    // iteration of safepoint insertion, we can just use that.
    if( isa<Instruction>(def) && cast<Instruction>(def)->getMetadata("is_base_value") ) {
      // If it's a base phi directly inserted by this algorithm, it's okay
      return true;
    }

    // Rather than duplicate a lot of complex logic, just use the base defining
    // value routine as a helper.  If the base defining value is the def (minus
    // known bitcasts, etc..), def is a base.  If it's not, it likely isn't.
    Value* base = findBaseDefiningValue(def);
    if( base == def &&
        !isa<PHINode>(def) &&
        !isa<SelectInst>(def) ) {
      return true;
    }
    return false;
  }
  
  /// helper function for insertPHIs -- is it valid for a new Phi use of the oldDef
  /// to be inserted into this BB?  Generally, this comes down to dominators
  /// via SSA properties, but there's a couple special cases.
  bool validLocationForNewPhi(DominatorTree& DT, Value* oldDef, BasicBlock* candidate) {
    assert( !isa<GlobalVariable>(oldDef) && "Can't be rewriting global");
    if( isa<Argument>(oldDef) ) {
      //use can be anywhere in function
      return true;
    }
    if( isa<UndefValue>(oldDef) ) {
      //mainly useful for bugpoint
      return true;
    }

    // The basic block is dominated by the original definition
    return DT.dominates(cast<Instruction>(oldDef), candidate);

    // Note: We do not need to handle PHIs just outside the dominated region
    // here.  This check is for *new* PHIs and we'll never need to add one
    // outside that region.  We may need to update *old* phis outside that region
  }

  bool atLeastOnePhiInBB(const set<PHINode*>& phis, const BasicBlock* BB) {
    for(set<PHINode*>::iterator itr = phis.begin(), end = phis.end();
        itr != end; itr++) {
      PHINode* use = *itr;
      if( use->getParent() == BB ) {
        return true;
      }
    }
    return false;
  }

  void updatePHIUses(DominatorTree& DT, Value* oldDef, std::map<BasicBlock*, Instruction*>& seen, set<PHINode*>& newPHIs, bool isNewPhi) {
    for(std::set<PHINode*>::iterator itr = newPHIs.begin(), end = newPHIs.end();
        itr != end; itr++) {
      PHINode* phi = *itr;
      // Check that the newly inserted PHIs don't continue outside the region
      // dominated by the original definition whose uses we're updating.
      assert( (!isNewPhi || validLocationForNewPhi(DT, oldDef, phi->getParent())) &&
              "Phi found at location incompatible with original definition");

      BasicBlock* BB = phi->getParent();
      if(!isNewPhi) {
        unsigned NumPHIValues = phi->getNumIncomingValues();
        assert( NumPHIValues > 0 && "zero input phis are illegal");
        assert( NumPHIValues == std::distance(pred_begin(BB), pred_end(BB)) && "wrong number of inputs");
      }
      
      assert( pred_begin(BB) != pred_end(BB) && "shouldn't reach block without predecessors");
      for(pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; PI++) {
        BasicBlock* pred = *PI;
        Value* def = NULL;
        if( seen.find(pred) != seen.end() &&
            NULL != seen[pred] ) {
          def = seen[pred];
          // Note: seen[pred] may actual dominate phi.  In particular,
          // backedges of loops with a def in the preheader make this really
          // common.  The phi is still needed.
          assert( isPotentiallyReachable(seen[pred]->getParent(), BB, &DT) && "sanity check - provably reachable by alg above.");
        } else if( seen.find(pred) != seen.end() &&
                   NULL == seen[pred] ) {
          // We encountered a kill here.  By assumption, the input is invalid
          // and doesn't matter.  This can happen when we insert one safepoint
          // which can reach another and the live set of the former is greater
          // than that of the later.  Right now, this requires interactions
          // between two insertion passes since (by accident) call safepoint
          // inserts safepoints in an order such that it alone will never
          // trigger this case.
          // Note: We use null rather than undef here for two reasons:
          // 1) it allows us to assert about seeing an undef while checking for
          // bases of relocation phis
          // 2) if we're wrong about this being unused, we'll hopefully fault cleanly.
          def = ConstantPointerNull::get(cast<PointerType>(phi->getType()));
        } else {
          assert( seen.find(pred) == seen.end() && "kill's should have been handled above");
          if( isNewPhi ) {

            //There are two cases here:
            // 1) reachable from a path which doesn't include the safepoint
            // 2) reachable from along a path from this safepoint which
            // includes another
            // The appropriate answers are different!  How resolve?
            
            // We didn't find this predecessor when walking forward from the
            // safepoint.  As a result, it must be a path which skips the
            // safepoint entirely.  Thus, the original def is correct.
            def = oldDef;
          } else {
            // We didn't see this edge, so the original phi input value is correct
            int idx = phi->getBasicBlockIndex(pred);
            assert( idx >= 0 && "incoming edge must exist!");
            def = phi->getIncomingValue(idx);
          }
          // Can't assert unreachable since routine is conservative about reachability
        }
        assert(def && "must have found a def");
        if( isNewPhi ) {
          phi->addIncoming(def, pred);
        } else {
          int idx = phi->getBasicBlockIndex(pred);
          assert( idx >= 0 && "incoming edge must exist!");

          // This check and set is effectively a path-dependent
          // replaceUsesOfWith.  We change phi nodes of the form
          //
          //  bblock:
          //    relocated value for %old on this path is %new
          //
          //    m = phi ... [ %old, %bblock ]
          //
          // to
          //
          //  bblock:
          //    relocated value for %old on this path is %new
          //
          //    m = phi ... [ %new, %bblock ]
          //
          // We need the check to filter phi nodes like
          //
          //  bblock:
          //    relocated value for %old on this path is %new
          //
          //    m = phi ... [ %unrelated_value, %bblock ]

          if (phi->getIncomingValue(idx) == oldDef) {
            phi->setIncomingValue(idx, def);
          }
        }
      }
      unsigned NumPHIValues = phi->getNumIncomingValues();
      assert( NumPHIValues > 0 && "zero input phis are illegal");
      assert( NumPHIValues == std::distance(pred_begin(BB), pred_end(BB)) && "wrong number of inputs");
    }

  }
}

/* Note: We settled on using a 'simple' data flow algorithm after experimenting
   with a dominance based alogrithm.  This can and should be improved, but the
   inductive invariants turned out to be suprisingly complicated and bugprone.
*/
void SafepointPlacementImpl::insertPHIsForNewDef(DominatorTree& DT, Value* oldDef, Instruction* newDef, std::pair<Instruction*, Instruction*> safepoint) {

  // PERF: most of this could be done for all the updated values at once.  It
  // should be fairly easy to rewrite this to work on vectors of old and new
  // defs.

  assert((isa<Argument>(oldDef) || isa<UndefValue>(oldDef) ||
          DT.dominates(cast<Instruction>(oldDef), newDef)) && "inserted def not well structured");

  // This check is conservative, it can return false when something is actually
  // a base, but it can not legally do the oposite.  
  bool isKnownBase = isKnownBasePointer(newDef);
    
  BasicBlock* newBB = newDef->getParent();
  if (TraceLSP) {
    errs() << "Relocating %" << oldDef->getName() << ", initial new def=%" << newDef->getName() << "\n";
  }

  // Note: We're only update the subset of uses which are phi-reachable from
  // the safepoint.  As a result, there can (and may) be uses of the original
  // new def which we do NOT update.

  // Update uses at the end of the safepoint, but do not mark the block as
  // visited.  We need to come back around and update uses before the
  // safepoint.
  BasicBlock::iterator past(newDef); past++;
  for(BasicBlock::iterator itr = past, end = newBB->end();
        itr != end; itr++) {
    Instruction* inst = &*itr;
    inst->replaceUsesOfWith(oldDef, newDef);

    if( isStatepoint(inst) ) {
      // All paths are dead - don't continue the search beyond the first block
      return;
    }
  }  
      
  // Now that we've reached the end of this block, we need to walk forward
  // and find all the places we may need to insert PHIs.  This loop serves
  // several purposes: it identifies the actual phi insertion sites, and it
  // tracks the defining phi (on exit!) for every block explored.  The later
  // is needed for getting the appropriate inputs to the newly inserted
  // phis.

  // block to dominating def _on exit_
  std::map<BasicBlock*, Instruction*> seen;
  // The basic block to be visited and the last dominating def _on entry_ of
  // that block
  typedef std::pair<BasicBlock*, Instruction*> frontier_node;
  std::vector<frontier_node> frontier;
  std::set<PHINode*> phis, newPHIs;

  // Unconditionally add all phi uses to be updated.  This will include some
  // outside of the reachable region, but we won't actually update those (since
  // the input edges will never be explored).  This allows us to terminate the
  // search when leaving the region dominated by oldDef without worrying about
  // phis in the blocks reachable immediately outside that region.
  for (Value::use_iterator I = oldDef->use_begin(), E = oldDef->use_end();
       I != E; I++) {
    Instruction* use = cast<Instruction>(*I);
    if( PHINode* phi = dyn_cast<PHINode>(use) ) {
      phis.insert( phi );
    }
  }

  // Push the successors of the current block, not the current block.  This
  // is key for dealing with a redefinition part way through a basic block
  // with a backedge which makes the block reachable from a successor.  We
  // may need to insert a PHI _above_ the newDef.  As a result, we have to
  // make sure the newBB gets considered as any other block would.
  for (succ_iterator PI = succ_begin(newBB), E = succ_end(newBB); PI != E; ++PI) {
    BasicBlock *Succ = *PI;

    // If the successor is outside of the region dominated by the original
    // definition, we definitely don't need to (and can't legally) insert
    // PHIs there or update values.  Don't add these to the worklist.
    if( !validLocationForNewPhi(DT, oldDef, Succ) ) {
      if (TraceLSP) {
        errs() << "Skipping %" << Succ->getName() << " as outside def region\n";
      }
      continue;
    }

    // The exiting def of this block is the entry def of the successors
    frontier.push_back( make_pair(Succ, newDef) );
  }
  while( !frontier.empty() ) {
    const frontier_node current = frontier.back();
    frontier.pop_back();
    BasicBlock* currentBB = current.first;
    Instruction* currentDef = current.second;
    if (TraceLSP) {
      errs() << "[TraceRelocations] entering %" << currentBB->getName() << " with value %"
             << currentDef->getName() << "\n";
    }

    assert( currentBB && currentDef && "Can't be null");
    assert( currentDef->getType() == newDef->getType() && "Types of definitions must match");
    if( seen.find(currentBB) != seen.end() ) {
      // we've seen this before, no need to revisit
      // we need the check here since the same item could be on the worklist
      // multiple times.  We could try to uniquify items, but that's
      // complicated and this is simply.
      continue;
    }

    // Note: We can have a phi use which is outside the region dominated by the
    // new def.  Ex:
    // A    D
    // | \  |
    // | SP /
    // | / /
    // B  /
    // | /
    // C <-- phi use here, unrelated to updates caused by SP
    // We can also have code which both uses the original value and a phi'd
    // value at once, etc.
    // A
    // | \
    // | B
    // | /
    // C - phi here
    // |
    // D <-- can use either original def or phi
    // Note: This example is _before_ safepoint insertion
    // Because of the later, we can't stop when we see the first phi.
    // Finally, we could end up with a use in a merge between two safepoints:
    // SP1 SP2
    //   \ /
    //    M < - use here
    // In this case, it is required that the merge node pick up both the
    // incoming relocations.  Since we don't neccessarily visit the path from
    // SP1 when updating from SP2, this requires that we track relocation phis
    // specially and reuse them.  Alternately, we could explore all paths for
    // every safepoint, but we'd rather not do that.

    // We should never revisit a block we've already decided to insert a phi in.
    assert( !atLeastOnePhiInBB(newPHIs, currentBB) && "If it already has a phi, why are we here?");

    bool isDeadPath = false;
    Instruction* exitDef = NULL;

    int num_preds = std::distance(pred_begin(currentBB), pred_end(currentBB));
    // The most trivial possible condition
    if( num_preds > 1 ) {

      // If we encounter a relocation phi, we know - by assumption - that there
      // are no uses of old def reachable from here.  As a result, we terminate
      // our search along all paths leading through this basic block.
      // If this block already contains a relocation phi, reuse it.  This is
      // required for correctness since we may not know the proper relocation
      // coming in along another path.  That path might not be reachable from
      // the current safepoint.
      // Note: This relies on the invariant that any relocation phi can only
      // relocate a single original definition.  (not oldDef, the original one
      // before any safepoints).  This is checked by the IR verifier.
      // (Any relocation phi in this block should be a kill at the moment,
      // we're being slightly more relaxed to allow removal of useles phis at a
      // later date.)
      for(BasicBlock::iterator itr = currentBB->begin(), end = currentBB->getFirstNonPHI();
          itr != end; itr++) {
        PHINode* phi = cast<PHINode>(&*itr);
        const unsigned NumPHIValues = phi->getNumIncomingValues();
        assert( NumPHIValues > 0 && "zero input phis are illegal");
        for (unsigned i = 0; i != NumPHIValues; ++i) {
          Value *InVal = phi->getIncomingValue(i);
          if( InVal == oldDef ) {
            if( phi->getMetadata("is_relocation_phi") ) {
              assert( !isDeadPath && "Can't have two relocation phis for same value in same basic block");
              isDeadPath = true;
            }              
            // We'll need to make sure that this use gets updated properly.
            // Note that the phi is live _even if_ the basic block is not due
            // to a relocation phi
            assert( phis.find(phi) != phis.end() && "must already be a use of oldDef");
            
            break;
          } //for NumPHIValues
          // Note: Could break here, but then the assertion above doesn't get
          // a chance to trigger.  Error checking is good.  :)
        }
      } //for uses
      if( isDeadPath ) {
        //TODO: There should be no other uses reachable from this phi node

        seen[currentBB] = NULL;
        continue;
        // Terminate this path in the search.  We encounted a safepoint, so by
        // assumption there are no uses which need updated beyond here.  If there
        // are, we want the verifier to yell loudly.  :)
      }


      // There's two parts of PHI insertion:
      // 1) Constructing the actual phi node
      // 2) Rewriting uses to use the new definition
      // 3) Figuring out what inputs the new phi has
      // We know enough to do 1 & 2, but not yet 3.  We'll have to come
      // back to that after finding all insert sites
      int num_preds = std::distance(pred_begin(currentBB), pred_end(currentBB));
      PHINode* phi = PHINode::Create(newDef->getType(), num_preds, "relocated", &currentBB->front());
      // Add a metadata annotation marking this as a relocation phi
      Value* const_1= ConstantInt::get(Type::getInt32Ty(newBB->getParent()->getParent()->getContext()), 1);         
      MDNode* md = MDNode::get(newBB->getParent()->getParent()->getContext(), const_1);
      phi->setMetadata("is_relocation_phi", md);
      if( isKnownBase ) {
        phi->setMetadata("is_base_value", md);
      }
      newPHIs.insert(phi);
      exitDef = phi;
    } else {
      exitDef = currentDef;
    }


    // Walk through the basic block to apply updates to non-phis (we'll handle
    // the phis later).  PERF: this could be easily optimized by a) checking to
    // see if there are any uses in this block and b) visiting only the uses
    for(BasicBlock::iterator itr = currentBB->getFirstNonPHI(), end = currentBB->end();
        itr != end; itr++) {
      Instruction* inst = &*itr;
      inst->replaceUsesOfWith(oldDef, exitDef);
      if( isStatepoint(inst) ) {
        // One slightly weird case that can happen is that we hit a safepoint
        // after encountering a relocation phi. If we're not careful, we'll hit
        // another join and insert a new relocation phi.  (i.e. we don't
        // recongize the relocated value as the current def)
        // Wc can end up with something like this:
        // SP - while propagating this safepoint
        // |
        // PHI - backedge from bottom phi
        // | \
        // | SP - we hit this safepoint
        // | /
        // PHI - and have to use it's value here

        // Note: We need to exclude the newBB from this critiria so that we can
        // visit the newBB block again.  We *know* - since we reached here -
        // that newDef is live out of this block
        if( currentBB != newBB ) {
          isDeadPath = true;
        }
        // It would be tempting to continue with the relocated value, BUT...
        // we could also encounter a safepoint which does not have this value
        // live and relocated.  The easiest correct thing to do is just
        // terminate the search (along this path) on this BB.
        break;
      }
    }
    if( isDeadPath ) {
      seen[currentBB] = NULL;
      continue;
      // Terminate this path in the search.  We encounted a safepoint, so by
      // assumption there are no uses which need updated beyond here.  If there
      // are, we want the verifier to yell loudly.  :)
    }
    
    // There's one special case where the block exit is not the newly
    // insert Phi.  If we're revisiting the block we inserted the
    // safepoint to, the exit def is the inserted one.  Note that all uses
    // after the safepoint in this block were already updated, so we don't
    // need to worry about that.  
    if( currentBB == newBB ) {
      exitDef = newDef;
    }

    assert( exitDef && "Exiting def can't be null");
    if (TraceLSP) {
      errs() << "[TraceRelocations] leaving %" << currentBB->getName() << " with value %" << exitDef->getName() << "\n";
    }
    assert(seen.find(currentBB) == seen.end() && "we're overwriting!");
    seen[currentBB] = exitDef;
      
    for (succ_iterator PI = succ_begin(currentBB), E = succ_end(currentBB); PI != E; ++PI) {
      BasicBlock *Succ = *PI;
      // optimization to avoid adding redundant work to worklist
      if( seen.find(Succ) != seen.end() ) {
        // we've seen this before, no need to revisit
        if (TraceLSP) {
          errs() << "Skipping %" << Succ->getName() << " as previously seen\n";
        }
        continue;
      }

      // If the successor is outside of the region dominated by the original
      // definition, we definitely don't need to (and can't legally) insert
      // PHIs there or update values.  Don't add these to the worklist.
      if( !validLocationForNewPhi(DT, oldDef, Succ) ) {
        if (TraceLSP) {
          errs() << "Skipping %" << Succ->getName() << " as outside def region\n";
        }
        continue;
      }
        
      // The exiting def of this block is the entry def of the successors
      frontier.push_back( make_pair(Succ, exitDef) );
    }
  }


  /* We assume that any basic block not in seen was not reachable from the
     safepoint.  Since we terminated the walk above early when we hit a
     previous safepoint, we need to make sure we mark any block reachable from
     that early safepoint as being invalid.  Otherwise, we'll try to insert a
     use of oldDef if there is a backedge.

     Note: This is a bit of hack.  It could (and possibly should) be integrated
     into the walk above.  There's enough complexity in that code already that
     I didn't want to risk another bug for what is honestly a pretty minor
     corner case which causes verifier failures, but not actual code gen issues.
  */
  /* scope */ {
    if (TraceLSP) {
      errs() << "Populating kills through reachable paths..\n";
    }
    std::vector<BasicBlock*> worklist;
    for (map<BasicBlock *, Instruction *>::iterator I = seen.begin(), E = seen.end(); I != E; ++I) {
      if( NULL == I->second ) {
        worklist.push_back(I->first);
      }
    }
    while( !worklist.empty() ) {
      BasicBlock* currentBB = worklist.back();
      worklist.pop_back();
      
      if( seen.find(currentBB) == seen.end() ) {
        seen[currentBB] = NULL;
      }
      
      for (succ_iterator PI = succ_begin(currentBB), E = succ_end(currentBB); PI != E; ++PI) {
        BasicBlock *Succ = *PI;
        if( seen.find(Succ) != seen.end() ) {
          if (TraceLSP) {
            errs() << "Skipping %" << Succ->getName() << " as previously seen\n";
          }
          continue;
        }
        
        if( !validLocationForNewPhi(DT, oldDef, Succ) ) {
          if (TraceLSP) {
            errs() << "Skipping %" << Succ->getName() << " as outside def region\n";
          }
          continue;
        }
        
        worklist.push_back(Succ);
      }
    }
  }

  // 'Visit' the initial block if we didn't encounter it in our traversal.  This
  // is required for the correctness of updatePHIUses since we inspect
  // predecessors.  We assume any input we haven't seen must provide the
  // original oldDef where the start block should provide the newDef.
  if( seen.find(newBB) == seen.end() ) {
    seen[newBB] = newDef;
  }

  if (TraceLSP) {
    errs() << "[TraceRelocations] map<BasicBlock *, Instruction *> seen == \n";
    for (map<BasicBlock *, Instruction *>::iterator I = seen.begin(), E = seen.end(); I != E; ++I) {
      errs() << "\tseen[%" << I->first->getName() << "] = %" << (I->second ? I->second->getName() : "NULL") << "\n";
    }
  }


  // As a final step, go through and update the incoming edges of all the
  // phis effected by the change.  This will be both our newly insert ones
  // and any relocation phis we encounted in our search. We use
  // the block exit definitions we gathered above.  We keep the two sets
  // separate to enable some useful error checking internally.
  updatePHIUses(DT, oldDef, seen, newPHIs, true);
  updatePHIUses(DT, oldDef, seen, phis, false);
}
