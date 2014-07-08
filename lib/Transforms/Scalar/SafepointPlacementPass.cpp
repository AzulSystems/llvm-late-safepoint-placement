/** Place garbage collection safepoints at appropriate locations
    in the IR.

    There are restrictions on the IR accepted.  We require that:
    - Pointer values may not be cast to integers and back.
    - Pointers to GC objects must be tagged with address space #1

    In addition to these fundemental limitations, we currently
    do not support:
    - safepoints at invokes
    - use of indirectbr
    - aggregate types which contain pointers to GC objects
    - pointers to GC objects stored in global variables, allocas,
      or at constant addresses
    - constant pointers to GC objects (other than null)
    - use of gc_root

    Patches welcome for the later class of items.

    This code is organized in two key concepts:
    - "parse point" - at these locations (all calls in the current
    implementation), the garbage collector must be able to inspect
    and modify all pointers to garbage collected objects.  The objects
    may be arbitrarily relocated and thus the pointers may be modified.
    - "poll" - this is a location where the compiled code needs to
    check (or poll) if the running thread needs to collaborate with
    the garbage collector by taking some action.  In this code, the
    checking condition and action are abstracted via a frontend
    provided "safepoint_poll" function.

    TODO: Documentation
    Explain why the various algorithms work.  Justify the pieces in at least
    high level detail.
 */
#define DEBUG_TYPE "safepoint-placement"
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
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

using namespace llvm;
using namespace std;

// Set this debugging variable to 1 to have the IR verified for consistency
// after every transform is complete.
cl::opt<bool> VerifyAllIR("spp-verify-all-ir", cl::init(true));

// Ignore oppurtunities to avoid placing safepoints on backedges, useful for
// validation
cl::opt<bool> AllBackedges("spp-all-backedges", cl::init(false));
// Only go as far as confirming base pointers exist, useful for fault isolation
cl::opt<bool> BaseRewriteOnly("spp-base-rewrite-only", cl::init(false));
// Add safepoints to all functions, not just the ones with attributes
extern cl::opt<bool> AllFunctions;
// Include deopt state in safepoints?
cl::opt<bool> UseVMState("spp-use-vm-state", cl::init(true));

// Print tracing output
cl::opt<bool> TraceLSP("spp-trace", cl::init(false));

// Print the liveset found at the insert location
cl::opt<bool> PrintLiveSet("spp-print-liveset", cl::init(false));
cl::opt<bool> PrintLiveSetSize("spp-print-liveset-size", cl::init(false));
// Print out the base pointers for debugging
cl::opt<bool> PrintBasePointers("spp-print-base-pointers", cl::init(false));
// Use alloca to emit relocation phis
cl::opt<bool> RelocViaAlloc("spp-reloc-via-alloca", cl::init(false));

// Bugpoint likes to reduce a crash into _any_ crash (including assertion
// failures due to configuration problems).  If we're reducing a 'real' crash
// under bugpoint, make simple configuration errors (which bugpoint introduces)
// look like normal behavior.
//#define USING_BUGPOINT
#ifdef USING_BUGPOINT
#define BUGPOINT_CLEAN_EXIT_IF(cond)                                           \
  if (true) {                                                                  \
    if ((cond)) {                                                              \
      errs() << "FATAL ERROR, exit cleanly for bugpoint\n";                    \
      exit(0);                                                                 \
    } else {                                                                   \
    }                                                                          \
  } else {                                                                     \
  }
#else
#define BUGPOINT_CLEAN_EXIT_IF(cond)                                           \
  if (true) {                                                                  \
  } else {                                                                     \
  }
#endif

static bool VMStateRequired() { return !AllFunctions && UseVMState; }

/* Note: PlaceBackedgeSafepointsImpl need to be instances of ModulePass, not
   LoopPass.  LoopPass is not allowed to do any cross module optimization
   (such as inlining).  The PassManager will run FunctionPasses (of which the
   Loop Pass Manager is one) in "some order" on all the relevant functions.
   In theory, the gc.safepoint_poll function could be being optimized
   (i.e. potentially invalid IR) when we attempt to inline it.  While in
   practice, passes aren't run in parallel, we did see an issue where we'd
   insert a safepoint into the poll function and *then* inline it.  Yeah,
   inlining after safepoint placement is utterly completely illegal and wrong.

   The only reason this works today is that a) we manually exclude
   safepoint_poll from consideration even under AllFunctions and b) we have
   barrier passes immediately before and after safepoint insertion.  This still
   isn't technically enough (LCSSA can modify loop edges in certian
   poll_safepoints, who knew?), but it mostly appears to work for the moment.

   Also, the whole LoopPass system is an utter mess.  Duplicating the loop
   based analysis here outside of a loop pass is non-trivial, but should be
   done ASAP.

   THIS REALLY NEEDS FIXED.
 */

namespace {

/** An analysis pass whose purpose is to identify each of the backedges in
    the function which require a safepoint poll to be inserted. */
struct PlaceBackedgeSafepointsImpl : public LoopPass {
  static char ID;

  /// The output of the pass - gives a list of each backedge (described by
  /// pointing at the branch) which need a poll inserted.
  std::vector<TerminatorInst *> PollLocations;

  PlaceBackedgeSafepointsImpl() : LoopPass(ID) {
    initializePlaceBackedgeSafepointsImplPass(*PassRegistry::getPassRegistry());
  }

  virtual bool runOnLoop(Loop *, LPPassManager &LPM);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    // needed for determining if the loop is finite
    AU.addRequired<ScalarEvolution>();
    // to ensure each edge has a single backedge
    // TODO: is this still required?
    AU.addRequiredID(LoopSimplifyID);

    // We no longer modify the IR at all in this pass.  Thus all
    // analysis are preserved.
    AU.setPreservesAll();
  }
};

cl::opt<bool> NoEntry("spp-no-entry", cl::init(false));
cl::opt<bool> NoCall("spp-no-call", cl::init(false));
cl::opt<bool> NoBackedge("spp-no-backedge", cl::init(false));

struct PlaceSafepoints : public ModulePass {
  static char ID; // Pass identification, replacement for typeid

  bool EnableEntrySafepoints;
  bool EnableBackedgeSafepoints;
  bool EnableCallSafepoints;

  PlaceSafepoints() : ModulePass(ID) {
    initializePlaceSafepointsPass(*PassRegistry::getPassRegistry());
    EnableEntrySafepoints = !NoEntry;
    EnableBackedgeSafepoints = !NoBackedge;
    EnableCallSafepoints = !NoCall;
  }
  virtual bool runOnModule(Module &M) {
    bool modified = false;
    for (Function &F : M) {
      modified |= runOnFunction(F);
    }
    return modified;
  }
  bool runOnFunction(Function &F);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    // We modify the graph wholesale (inlining, block insertion, etc).  We
    // preserve nothing at the moment.  We could potentially preserve dom tree
    // if that was worth doing
  }
};

struct RemoveFakeVMStateCalls : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  RemoveFakeVMStateCalls() : FunctionPass(ID) {
    initializeRemoveFakeVMStateCallsPass(*PassRegistry::getPassRegistry());
  }
  virtual bool runOnFunction(Function &F) {
    // Track the calls and function definitions to be removed
    std::vector<CallInst *> instToRemove;
    std::set<Function *> funcToRemove;
    for (inst_iterator itr = inst_begin(F), end = inst_end(F); itr != end;
         itr++) {
      if (isJVMState(&*itr)) {
        CallInst *CI = cast<CallInst>(&*itr);
        instToRemove.push_back(CI);
        funcToRemove.insert(CI->getCalledFunction());
      }
    }

    // remove all the calls (i.e. uses of functions)
    for (CallInst *CI : instToRemove) {
      // Remove the use holding this call in place
      assert(CI->hasOneUse() && "must have exactly one use");
      StoreInst *User = cast<StoreInst>(*CI->user_begin());
      assert(isJVMStateAnchorInstruction(User));
      User->eraseFromParent();
      assert(CI->use_empty() && "should be no uses left");

      // Remove the call itself
      CI->eraseFromParent();
    }

    // remove the functions which are now dead - note that the use of a set is
    // required since calls can be duplicated by the optimizer
    for (Function *F : funcToRemove) {
      // The conditional is a safety check to handle another use which is
      // somehow hanging around.
      if (F->use_empty()) {
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

namespace {
/** The type of the internal cache used inside the findBasePointers family
    of functions.  From the callers perspective, this is an opaque type and
    should not be inspected.

    In the actual implementation this caches two relations:
    - The base relation itself (i.e. this pointer is based on that one)
    - The base defining value relation (i.e. before base_phi insertion)
    Generally, after the execution of a full findBasePointer call, only the
    base relation will remain.  Internally, we add a mixture of the two
    types, then update all the second type to the first type
*/
typedef std::map<Value *, Value *> DefiningValueMapTy;
}

// The following declarations call out the key steps of safepoint placement and
// summarize their preconditions, postconditions, and side effects.  This is
// best read as a summary; if you need detail on implementation, dig into the
// actual implementations below.
namespace SafepointPlacementImpl {

struct PartiallyConstructedSafepointRecord {
  /// The set of values known to be live accross this safepoint
  std::set<llvm::Value *> liveset;

  /// Mapping from live pointers to a base-defining-value
  std::map<llvm::Value *, llvm::Value *> base_pairs;

  /// Any new values which were added to the IR during base pointer analysis
  /// for this safepoint
  std::set<llvm::Value *> newInsertedDefs;

  /// The bounds of the inserted code for the safepoint
  std::pair<Instruction *, Instruction *> safepoint;

  /// The result of the safepointing call (or NULL)
  Value *result;

  void verify() {}
};

/// Find the initial live set. Note that due to base pointer
/// insertion, the live set may be incomplete.
void analyzeParsePointLiveness(DominatorTree &DT, const CallSite &CS,
                               PartiallyConstructedSafepointRecord &result);

/// Find the required based pointers (and adjust the live set) for the given
/// parse point.
void findBasePointers(DominatorTree &DT, DefiningValueMapTy &DVCache,
                      const CallSite &CS,
                      PartiallyConstructedSafepointRecord &result);

/// Check for liveness of items in the insert defs and add them to the live
/// and base pointer sets
void fixupLiveness(DominatorTree &DT, const CallSite &CS,
                   const std::set<Value *> &allInsertedDefs,
                   PartiallyConstructedSafepointRecord &result);

/// Insert a safepoint (parse point) at the given call instruction.  Does not
/// do relocation and does not remove the existing call.  That's handled by
/// the caller.
void InsertSafepoint(DominatorTree &DT, const CallSite &CS, CallInst *vmstate,
                     PartiallyConstructedSafepointRecord &result);

// Insert a safepoint poll immediately before the given instruction.  Does
// not handle the parsability of state at the runtime call, that's the
// callers job.
void InsertSafepointPoll(DominatorTree &DT, Instruction *after,
                         std::vector<CallSite> &ParsePointsNeeded /*rval*/);

bool isGCLeafFunction(const CallSite &CS);

bool needsStatepoint(const CallSite &CS) {
  if (isGCLeafFunction(CS))
    return false;
  if (CS.isCall()) {
    // Why the hell is inline ASM modeled as a call instruction?
    CallInst *call = cast<CallInst>(CS.getInstruction());
    if (call->isInlineAsm())
      return false;
  }
  if (isStatepoint(CS) || isGCRelocate(CS) || isGCResult(CS)) {
    // In case we run backedge, then call safepoint placement...
    return false;
  }
  return true;
}

bool isLiveAtSafepoint(Instruction *term, Instruction *use, Value &inst,
                       DominatorTree &DT, LoopInfo *LI);

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
void findLiveGCValuesAtInst(Instruction *term, BasicBlock *pred,
                            DominatorTree &DT, LoopInfo *LI,
                            std::set<llvm::Value *> &liveValues);

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
void findBasePointers(const std::set<llvm::Value *> &live,
                      std::map<llvm::Value *, llvm::Value *> &base_pairs,
                      DominatorTree *DT, DefiningValueMapTy &DVCache,
                      std::set<llvm::Value *> &newInsertedDefs);

CallInst *findVMState(llvm::Instruction *term, DominatorTree *DT);

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
void CreateSafepoint(const CallSite &CS, /* to replace */
                     llvm::CallInst *vm_state,
                     const std::vector<llvm::Value *> &basePtrs,
                     const std::vector<llvm::Value *> &liveVariables,
                     struct PartiallyConstructedSafepointRecord &result);

/// This routine walks the CFG and inserts PHI nodes as needed to handle a
/// new definition which is replacing an old definition at a location where
/// there didn't use to be a use.  The Value being replaced need not be an
/// instruction (it can be an alloc, or argument for instance), but the
/// replacement definition must be an Instruction.
void insertPHIsForNewDef(DominatorTree &DT, Function &F, Value *oldDef);

// do all the relocation update via allocas and mem2reg
void relocationViaAlloca(
    Function &F, DominatorTree &DT, const std::vector<Value *> &live,
    const std::vector<struct PartiallyConstructedSafepointRecord> &records);
}

using namespace SafepointPlacementImpl;

namespace {
/// Returns true if this loop is known to terminate in a finite number of
/// iterations.  Note that this function may return false for a loop which
/// does actual terminate in a finite constant number of iterations due to
/// conservatism in the analysis.
bool mustBeFiniteCountedLoop(Loop *L, ScalarEvolution *SE) {
  unsigned TripCount = 0;

  // Currently, only handles loops with a single backedge (common case)
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

void addBasesAsLiveValues(std::set<Value *> &liveset,
                          std::map<Value *, Value *> &base_pairs) {
  // Identify any base pointers which are used in this safepoint, but not
  // themselves relocated.  We need to relocate them so that later inserted
  // safepoints can get the properly relocated base register.
  std::set<Value *> missing;
  for (Value *live : liveset) {
    assert(base_pairs.find(live) != base_pairs.end());
    Value *base = base_pairs[live];
    assert(base);
    if (liveset.find(base) == liveset.end()) {
      assert(base_pairs.find(base) == base_pairs.end());
      // uniqued by set insert
      missing.insert(base);
    }
  }

  // Note that we want these at the end of the list, otherwise
  // register placement gets screwed up once we lower to STATEPOINT
  // instructions.  This is an utter hack, but there doesn't seem to be a
  // better one.
  for (Value *base : missing) {
    assert(base);
    liveset.insert(base);
    base_pairs[base] = base;
  }
  assert(liveset.size() == base_pairs.size());
}
void scanOneBB(Instruction *start, Instruction *end,
               std::vector<CallInst *> &calls, std::set<BasicBlock *> &seen,
               std::vector<BasicBlock *> &worklist) {
  for (BasicBlock::iterator itr(start);
       itr != start->getParent()->end() && itr != BasicBlock::iterator(end);
       itr++) {
    if (CallInst *CI = dyn_cast<CallInst>(&*itr)) {
      calls.push_back(CI);
    }
    // FIXME: This code does not handle invokes
    assert(!dyn_cast<InvokeInst>(&*itr) &&
           "support for invokes in poll code needed");
    // Only add the successor blocks if we reach the terminator instruction
    // without encountering end first
    if (itr->isTerminator()) {
      BasicBlock *BB = itr->getParent();
      for (succ_iterator PI = succ_begin(BB), E = succ_end(BB); PI != E; ++PI) {
        BasicBlock *Succ = *PI;
        if (seen.count(Succ) == 0) {
          worklist.push_back(Succ);
          seen.insert(Succ);
        }
      }
    }
  }
}
void scanInlinedCode(Instruction *start, Instruction *end,
                     std::vector<CallInst *> &calls,
                     std::set<BasicBlock *> &seen) {
  calls.clear();
  std::vector<BasicBlock *> worklist;
  seen.insert(start->getParent());
  scanOneBB(start, end, calls, seen, worklist);
  while (!worklist.empty()) {
    BasicBlock *BB = worklist.back();
    worklist.pop_back();
    scanOneBB(&*BB->begin(), end, calls, seen, worklist);
  }
}
}

/// Given a set of patch points which need to be parsable, turn them in to
/// statepoints.  WARNING: Destroys the CallSites, they no longer exist!
static bool insertParsePoints(Function &F, DominatorTree &DT,
                              std::vector<CallSite> &toUpdate);

bool PlaceBackedgeSafepointsImpl::runOnLoop(Loop *L, LPPassManager &LPM) {
  ScalarEvolution *SE = &getAnalysis<ScalarEvolution>();

  // Loop through all predecessors of the loop header and identify all
  // backedges.  We need to place a safepoint on every backedge (potentially).
  // Note: Due to LoopSimplify there should only be one.  Assert?  Or can we
  // relax this?
  BasicBlock *header = L->getHeader();

  Function *Func = header->getParent();
  assert(Func);
  bool shouldRun =
      AllFunctions || Func->getFnAttribute("gc-add-backedge-safepoints")
                          .getValueAsString()
                          .equals("true");
  if (shouldRun && Func->getName().equals("gc.safepoint_poll")) {
    assert(AllFunctions && "misconfiguration");
    // go read the module pass comment above
    shouldRun = false;
    errs() << "WARNING: Ignoring (illegal) request to place safepoints in "
              "gc.safepoint_poll\n";
  }
  if (!shouldRun) {
    return false;
  }

  bool modified = false;
  for (pred_iterator PI = pred_begin(header), E = pred_end(header); PI != E;
       PI++) {
    BasicBlock *pred = *PI;
    if (!L->contains(pred)) {
      // This is not a backedge, it's coming from outside the loop
      continue;
    }

    // Make a policy decision about whether this loop needs a safepoint or
    // not.  Place early for performance.  Could run later for some validation,
    // but at great cost performance wise.
    if (!AllBackedges) {
      if (mustBeFiniteCountedLoop(L, SE)) {
        if (TraceLSP)
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
    TerminatorInst *term = pred->getTerminator();

    if (TraceLSP) {
      errs() << "[LSP] terminator instruction: ";
      term->dump();
    }

    PollLocations.push_back(term);
  }

  return modified;
}

static Instruction *findLocationForEntrySafepoint(Function &F,
                                                  DominatorTree &DT) {
  bool shouldRun =
      AllFunctions ||
      F.getFnAttribute("gc-add-entry-safepoints").getValueAsString().equals(
          "true");
  if (shouldRun && F.getName().equals("gc.safepoint_poll")) {
    assert(AllFunctions && "misconfiguration");
    // go read the module pass comment above
    shouldRun = false;
    errs() << "WARNING: Ignoring (illegal) request to place safepoints in "
              "gc.safepoint_poll\n";
  }
  if (!shouldRun) {
    return NULL;
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
    return NULL;
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
    BasicBlock *nextBB = currentBB->getUniqueSuccessor();
    if (!nextBB) {
      // split node
      break;
    }
    if (NULL == nextBB->getUniquePredecessor()) {
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
  TerminatorInst *term = currentBB->getTerminator();
  return term;
}

/// Identify the list of call sites which need to be have parseable state
static void findCallSafepoints(Function &F,
                               std::vector<CallSite> &Found /*rval*/);

static void findCallSafepoints(Function &F,
                               std::vector<CallSite> &Found /*rval*/) {
  assert(Found.empty() && "must be empty!");
  bool shouldRun =
      AllFunctions ||
      F.getFnAttribute("gc-add-call-safepoints").getValueAsString().equals(
          "true");
  if (shouldRun && F.getName().equals("gc.safepoint_poll")) {
    assert(AllFunctions && "misconfiguration");
    // go read the module pass comment above
    shouldRun = false;
    errs() << "WARNING: Ignoring (illegal) request to place safepoints in "
              "gc.safepoint_poll\n";
  }
  if (!shouldRun) {
    return;
  }

  for (inst_iterator itr = inst_begin(F), end = inst_end(F); itr != end;
       itr++) {
    Instruction *inst = &*itr;

    if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
      CallSite CS(inst);

      // No safepoint needed or wanted
      if (!needsStatepoint(CS)) {
        continue;
      }

      Found.push_back(CS);
    }
  }
}

/// Implement a unique function which doesn't require we sort the input
/// vector.  Doing so has the effect of changing the output of a couple of
/// tests in ways which make them less useful in testing fused safepoints.
/// TODO: remove this once fused safepoints is working and fix the tests
template <typename T> static void unique_unsorted(std::vector<T> &vec) {
  std::set<T> seen;
  std::vector<T> tmp;
  vec.reserve(vec.size());
  std::swap(tmp, vec);
  for (T &el : tmp) {
    if (seen.insert(el).second) {
      vec.push_back(el);
    }
  }
}

static Function *getUseHolder(Function &F) {
  FunctionType *ftype =
      FunctionType::get(Type::getVoidTy(F.getParent()->getContext()), true);
  Function *Func =
      cast<Function>(F.getParent()->getOrInsertFunction("__tmp_use", ftype));
  return Func;
}

static bool insertParsePoints(Function &F, DominatorTree &DT,
                              std::vector<CallSite> &toUpdate) {
  // Unique the vectors since we can end up with duplicates if we scan the call
  // site for call safepoints after we add it for entry or backedge.  The
  // only reason we need tracking at all is that some functions might have
  // polls but not call safepoints and thus we might miss marking the runtime
  // calls for the polls. (This is useful in test cases!)
  unique_unsorted(toUpdate);

  // sanity check the input
  for (const CallSite &CS : toUpdate) {
    assert(CS.getInstruction()->getParent()->getParent() == &F);
  }

  // A list of dummy calls added to the IR to keep various values obviously
  // live in the IR.  We'll remove all of these when done.
  std::vector<CallInst *> holders;

  // Insert a dummy call with all of the arguments to the vm_state we'll need
  // for the actual safepoint insertion.  This ensures those arguments are held
  // live over safepoints between the current jvmstate and the eventual use
  // we'll insert below.
  if (VMStateRequired()) {
    holders.reserve(holders.size() + toUpdate.size());
    for (const CallSite &CS : toUpdate) {
      assert(CS.isCall() && "implement invoke here");

      // This must tbe the same jvmstate we find later
      CallInst *vm_state = findVMState(CS.getInstruction(), &DT);
      BUGPOINT_CLEAN_EXIT_IF(!vm_state);
      assert(vm_state && "must find vm state or be scanning c++ source code");

      // Create a clone then change the name for readability
      CallInst *holder = cast<CallInst>(vm_state->clone());
      holder->setCalledFunction(getUseHolder(F));

      // Insert the holder right after the parsepoint
      BasicBlock::iterator next(CS.getInstruction());
      next++;
      CS.getInstruction()->getParent()->getInstList().insert(next, holder);
      holders.push_back(holder);
    }
  }

  std::vector<struct PartiallyConstructedSafepointRecord> records;
  records.reserve(toUpdate.size());
  // A) Identify all gc pointers which are staticly live at the given call
  // site.
  for (const CallSite &CS : toUpdate) {
    struct PartiallyConstructedSafepointRecord info;
    analyzeParsePointLiveness(DT, CS, info);
    records.push_back(info);
  }
  assert(records.size() == toUpdate.size());

  // B) Find the base pointers for each live pointer
  /* scope for caching */ {
    // Cache the 'defining value' relation used in the computation and
    // insertion of base phis and selects.  This ensures that we don't insert
    // large numbers of duplicate base_phis.
    DefiningValueMapTy DVCache;

    for (size_t i = 0; i < records.size(); i++) {
      struct PartiallyConstructedSafepointRecord &info = records[i];
      CallSite &CS = toUpdate[i];
      findBasePointers(DT, DVCache, CS, info);
    }
  } // end of cache scope
  assert(records.size() == toUpdate.size());

  // The base phi insertion logic (for any safepoint) may have inserted new
  // instructions which are now live at some safepoint.  The simplest such
  // example is:
  // loop:
  //   phi a  <-- will be a new base_phi here
  //   safepoint 1 <-- that needs to be live here
  //   gep a + 1
  //   safepoint 2
  //   br loop
  std::set<llvm::Value *> allInsertedDefs;
  for (const struct PartiallyConstructedSafepointRecord &info : records) {
    allInsertedDefs.insert(info.newInsertedDefs.begin(),
                           info.newInsertedDefs.end());
  }
  // We insert some dummy calls after each safepoint to definitely hold live
  // the base pointers which were identified for that safepoint.  We'll then
  // ask liveness for _every_ base inserted to see what is now live.  Then we
  // remove the dummy calls.
  holders.reserve(holders.size() + records.size());
  for (size_t i = 0; i < records.size(); i++) {
    struct PartiallyConstructedSafepointRecord &info = records[i];
    CallSite &CS = toUpdate[i];
    Function *Func = getUseHolder(F);

    std::vector<Value *> bases;
    for (const std::pair<Value *, Value *> &I : info.base_pairs) {
      bases.push_back(I.second);
    }

    assert(CS.isCall() && "implement invoke here");

    BasicBlock::iterator next(CS.getInstruction());
    next++;
    CallInst *base_holder = CallInst::Create(Func, bases, "", next);
    holders.push_back(base_holder);
  }

  for (size_t i = 0; i < records.size(); i++) {
    struct PartiallyConstructedSafepointRecord &info = records[i];
    CallSite &CS = toUpdate[i];

    fixupLiveness(DT, CS, allInsertedDefs, info);
  }
  for (CallInst *Holder : holders) {
    Holder->eraseFromParent();
  }
  holders.clear();

  // Now run through and insert the safepoints, but do _NOT_ update or remove
  // any existing uses.  We have references to live variables that need to
  // survive to the last iteration of this loop.
  for (size_t i = 0; i < records.size(); i++) {
    struct PartiallyConstructedSafepointRecord &info = records[i];
    CallSite &CS = toUpdate[i];
    // locate the defining VM state object for this location
    CallInst *vm_state = NULL;
    if (VMStateRequired()) {
      vm_state = findVMState(CS.getInstruction(), &DT);
      BUGPOINT_CLEAN_EXIT_IF(!vm_state);
      assert(vm_state && "must find vm state or be scanning c++ source code");
      // Note: There is an implicit assumption here that values in the VM state
      // are live at the statepoint if-and-only-if they are live at the VM
      // state.  We duplicate jvm_states before each possible statepoint right
      // before this pass runs, so this should hold.
      // As a result of this assumption, we don't need to adjust liveness for
      // values at statepoints based on what jvm_states other statepoints might
      // need.  This is an important simpllication.
    }
    // Note: This deletes the instruction refered to by the CallSite!
    InsertSafepoint(DT, CS, vm_state, info);
    info.verify();
  }

  // Adjust all users of the old call sites to use the new ones instead
  for (size_t i = 0; i < records.size(); i++) {
    struct PartiallyConstructedSafepointRecord &info = records[i];
    CallSite &CS = toUpdate[i];
    BasicBlock *BB = CS.getInstruction()->getParent();
    // If there's a result (which might be live at another safepoint), update it
    if (info.result) {
      // Replace all uses with the new call
      CS.getInstruction()->replaceAllUsesWith(info.result);
    }

    // Now that we've handled all uses, remove the original call itself
    // Note: The insert point can't be the deleted instruction!
    CS.getInstruction()->eraseFromParent();
    /* scope */ {
      // trip an assert if somehow this isn't a terminator
      TerminatorInst *TI = BB->getTerminator();
      (void)TI;
      assert((CS.isCall() || TI == info.safepoint.first) &&
             "newly insert invoke is not terminator?");
    }
  }

  toUpdate.clear(); // prevent accident use of invalid CallSites

  if (VerifyAllIR) {
    // Did we generate valid IR?  Safepoint invariants don't yet hold
    verifyFunction(F);
  }

  // Do all the fixups of the original live variables to their relocated selves
  std::vector<Value *> live;
  for (const struct PartiallyConstructedSafepointRecord &info : records) {
    // We can't simply save the live set from the original insertion.  One of
    // the live values might be the result of a call which needs a safepoint.
    // That Value* no longer exists and we need to use the new gc_result.
    // Thankfully, the liveset is embedded in the statepoint (and updated), so
    // we just grab that.
    Statepoint statepoint(info.safepoint.first);
    live.insert(live.end(), statepoint.gc_args_begin(),
                statepoint.gc_args_end());
  }
  unique_unsorted(live);

  if (RelocViaAlloc) {
    relocationViaAlloca(F, DT, live, records);
  } else {
    for (Value *Live : live) {
      insertPHIsForNewDef(DT, F, Live);
    }
  }

  // Verify the result
  if (VerifyAllIR) {
    // post condition (safepoint invariants hold)
    verifyFunction(F);
    verifySafepointIR(F);
  }
  return !records.empty();
}

// TODO:
// - separate the analysis into it's own step
// - convert the for-safepoint loop into a per-phase, per safepoint loop
bool PlaceSafepoints::runOnFunction(Function &F) {

  if (F.isDeclaration() || F.begin() == F.end()) {
    // This is a declaration, nothing to do.  Must exit early to avoid crash in
    // dom tree calculation
    return false;
  }

  if (VerifyAllIR) {
    // precondition check, valid IR, safepoint invariants not yet established
    verifyFunction(F);
  }

  // TODO: We can be less aggressive about inserting polls here if we know the
  // loop contains a call which contains a poll.

  // STEP 1 - Insert the safepoint polling locations.  We do not need to
  // actually insert parse points yet.  That will be done for all polls and
  // calls in a single pass.

  bool modified = false;

  // Note: With the migration, we need to recompute this for each 'pass'.  Once
  // we merge these, we'll do it once before the analysis
  DominatorTree DT;

  std::vector<CallSite> ParsePointNeeded;

  if (EnableBackedgeSafepoints) {
    // Construct a pass manager to run the LoopPass backedge logic.  We
    // need the pass manager to handle scheduling all the loop passes
    // appropriately.  Doing this by hand is painful and just not worth messing
    // with for the moment.
    FunctionPassManager FPM(F.getParent());
    PlaceBackedgeSafepointsImpl *PBS = new PlaceBackedgeSafepointsImpl();
    FPM.add(PBS);
    // Note: While the analysis pass itself won't modify the IR, LoopSimplify
    // (which it depends on) may.  i.e. analysis must be recalculated after run
    FPM.run(F);

    // We preserve dominance information when inserting the poll, otherwise
    // we'd have to recalculate this on every insert
    DT.recalculate(F);

    // Insert a poll at each point the analysis pass identified
    for (TerminatorInst *term : PBS->PollLocations) {
      // We are inserting a poll, the function is modified
      modified = true;

      // VM State handling is handled when making the runtime call sites
      // parsable
      std::vector<CallSite> ParsePoints;
      InsertSafepointPoll(DT, term, ParsePoints);

      // Record the parse points for later use
      ParsePointNeeded.insert(ParsePointNeeded.end(), ParsePoints.begin(),
                              ParsePoints.end());
    }
  }

  if (EnableEntrySafepoints) {
    DT.recalculate(F);
    Instruction *term = findLocationForEntrySafepoint(F, DT);
    if (!term) {
      // policy choice not to insert?
    } else {
      std::vector<CallSite> RuntimeCalls;
      InsertSafepointPoll(DT, term, RuntimeCalls);
      modified = true;
      ParsePointNeeded.insert(ParsePointNeeded.end(), RuntimeCalls.begin(),
                              RuntimeCalls.end());
    }
  }

  if (EnableCallSafepoints) {
    DT.recalculate(F);
    std::vector<CallSite> Calls;
    findCallSafepoints(F, Calls);
    ParsePointNeeded.insert(ParsePointNeeded.end(), Calls.begin(), Calls.end());
  }
  // Any parse point (no matter what source) will be handled here
  DT.recalculate(F); // Needed?
  modified |= insertParsePoints(F, DT, ParsePointNeeded);

  return modified;
}

char PlaceBackedgeSafepointsImpl::ID = 0;
char PlaceSafepoints::ID = 0;
char RemoveFakeVMStateCalls::ID = 0;

ModulePass *llvm::createPlaceSafepointsPass() { return new PlaceSafepoints(); }

FunctionPass *llvm::createRemoveFakeVMStateCallsPass() {
  return new RemoveFakeVMStateCalls();
}

INITIALIZE_PASS_BEGIN(PlaceBackedgeSafepointsImpl,
                      "place-backedge-safepoints-impl",
                      "Place Backedge Safepoints", false, false)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_END(PlaceBackedgeSafepointsImpl,
                    "place-backedge-safepoints-impl",
                    "Place Backedge Safepoints", false, false)

INITIALIZE_PASS_BEGIN(PlaceSafepoints, "place-safepoints", "Place Safepoints",
                      false, false)
INITIALIZE_PASS_END(PlaceSafepoints, "place-safepoints", "Place Safepoints",
                    false, false)

INITIALIZE_PASS_BEGIN(RemoveFakeVMStateCalls, "remove-fake-vmstate-calls",
                      "Remove VM state calls", false, false)
INITIALIZE_PASS_END(RemoveFakeVMStateCalls, "remove-fake-vmstate-calls",
                    "Remove VM state calls", false, false)

bool SafepointPlacementImpl::isGCLeafFunction(const CallSite &CS) {
  Instruction *inst = CS.getInstruction();
  if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(inst)) {
    switch (II->getIntrinsicID()) {
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
      break; // fall through to generic call handling
    }
  }

  // If this function is marked explicitly as a leaf call, we don't need to
  // place a safepoint of it.  In fact, for correctness we *can't* in many
  // cases.  Note: Indirect calls return Null for the called function,
  // these obviously aren't runtime functions with attributes
  const Function *F = CS.getCalledFunction();
  bool isLeaf =
      F &&
      F->getFnAttribute("gc-leaf-function").getValueAsString().equals("true");
  if (isLeaf) {
    return true;
  }
  return false;
}

void SafepointPlacementImpl::InsertSafepointPoll(
    DominatorTree &DT, Instruction *term,
    std::vector<CallSite> &ParsePointsNeeded /*rval*/) {
  Module *M = term->getParent()->getParent()->getParent();
  assert(M);

  // Inline the safepoint poll implementation - this will get all the branch,
  // control flow, etc..  Most importantly, it will introduce the actual slow
  // path call - where we need to insert a safepoint (parsepoint).
  FunctionType *ftype =
      FunctionType::get(Type::getVoidTy(M->getContext()), false);
  assert(ftype && "null?");
  // Note: This cast can fail if there's a function of the same name with a
  // different type inserted previously
  Function *F =
      dyn_cast<Function>(M->getOrInsertFunction("gc.safepoint_poll", ftype));
  BUGPOINT_CLEAN_EXIT_IF(!(F && F->begin() != F->end()));
  assert(F && F->begin() != F->end() && "definition must exist");
  CallInst *poll = CallInst::Create(F, "", term);

  if (VerifyAllIR) {
    verifyFunction(*term->getParent()->getParent());
  }

  // Record some information about the call site we're replacing
  BasicBlock *OrigBB = term->getParent();
  BasicBlock::iterator before(poll), after(poll);
  bool isBegin(false);
  if (before == term->getParent()->begin()) {
    isBegin = true;
  } else {
    before--;
  }
  after++;
  assert(after != poll->getParent()->end() && "must have successor");
  assert(DT.dominates(before, after) && "trivially true");

  // do the actual inlining
  InlineFunctionInfo IFI;
  bool inlineStatus = InlineFunction(poll, IFI);
  assert(inlineStatus && "inline must succeed");

  // Check post conditions
  assert(IFI.StaticAllocas.empty() && "can't have allocs");

  std::vector<CallInst *> calls; // new calls
  std::set<BasicBlock *> BBs;    // new BBs + insertee
  // Include only the newly inserted instructions, Note: begin may not be valid
  // if we inserted to the beginning of the basic block
  BasicBlock::iterator start;
  if (isBegin) {
    start = OrigBB->begin();
  } else {
    start = before;
    start++;
  }

  // If your poll function includes an unreachable at the end, that's not
  // valid.  Bugpoint likes to create this, so check for it.
  BUGPOINT_CLEAN_EXIT_IF(!isPotentiallyReachable(&*start, &*after, NULL, NULL));
  assert(isPotentiallyReachable(&*start, &*after, NULL, NULL) &&
         "malformed poll function");

  scanInlinedCode(&*(start), &*(after), calls, BBs);

  // Recompute since we've invalidated cached data.  Conceptually we
  // shouldn't need to do this, but implementation wise we appear to.  Needed
  // so we can insert safepoints correctly.
  // TODO: update more cheaply
  DT.recalculate(*after->getParent()->getParent());

  if (VerifyAllIR) {
    verifyFunction(*term->getParent()->getParent());
  }

  BUGPOINT_CLEAN_EXIT_IF(calls.empty());
  assert(!calls.empty() && "slow path not found for safepoint poll");

  // Record the fact we need a parsable state at the runtime call contained in
  // the poll function.  This is required so that the runtime knows how to
  // parse the last frame when we actually take  the safepoint (i.e. execute
  // the slow path)
  assert(ParsePointsNeeded.empty());
  for (CallInst *Call : calls) {
    // No safepoint needed or wanted
    if (!needsStatepoint(Call)) {
      continue;
    }

    // These are likely runtime calls.  Should we assert that via calling
    // convention or something?
    ParsePointsNeeded.push_back(CallSite(Call));
  }
  assert(ParsePointsNeeded.size() <= calls.size());
}

namespace {
struct name_ordering {
  Value *base;
  Value *derived;
  bool operator()(name_ordering const &a, name_ordering const &b) {
    return -1 == a.derived->getName().compare(b.derived->getName());
  }
};
void stablize_order(std::vector<Value *> &basevec,
                    std::vector<Value *> &livevec) {
  assert(basevec.size() == livevec.size());

  vector<name_ordering> temp;
  for (size_t i = 0; i < basevec.size(); i++) {
    name_ordering v;
    v.base = basevec[i];
    v.derived = livevec[i];
    temp.push_back(v);
  }
  std::sort(temp.begin(), temp.end(), name_ordering());
  for (size_t i = 0; i < basevec.size(); i++) {
    basevec[i] = temp[i].base;
    livevec[i] = temp[i].derived;
  }
}

bool order_by_name(llvm::Value *a, llvm::Value *b) {
  if (a->hasName() && b->hasName()) {
    return -1 == a->getName().compare(b->getName());
  } else if (a->hasName() && !b->hasName()) {
    return true;
  } else if (!a->hasName() && b->hasName()) {
    return false;
  } else {
    // Better than nothing, but not stable
    return a < b;
  }
}
}

void SafepointPlacementImpl::analyzeParsePointLiveness(
    DominatorTree &DT, const CallSite &CS,
    SafepointPlacementImpl::PartiallyConstructedSafepointRecord &result) {
  Instruction *inst = CS.getInstruction();

  BasicBlock *BB = inst->getParent();
  std::set<llvm::Value *> liveset;
  findLiveGCValuesAtInst(inst, BB, DT, NULL, liveset);

  if (PrintLiveSet) {
    // Note: This output is used by several of the test cases
    // The order of elemtns in a set is not stable, put them in a vec and sort
    // by name
    std::vector<Value *> temp;
    temp.insert(temp.end(), liveset.begin(), liveset.end());
    std::sort(temp.begin(), temp.end(), order_by_name);
    errs() << "Live Variables:\n";
    for (const Value *Val : temp) {
      errs() << " " << Val->getName() << "\n";
      //(*itr)->dump();
    }
  }
  if (PrintLiveSetSize) {
    errs() << "Safepoint For: " << CS.getCalledValue()->getName() << "\n";
    errs() << "Number live values: " << liveset.size() << "\n";
  }
  result.liveset = liveset;
}

void SafepointPlacementImpl::findBasePointers(
    DominatorTree &DT, DefiningValueMapTy &DVCache, const CallSite &CS,
    SafepointPlacementImpl::PartiallyConstructedSafepointRecord &result) {
  std::map<llvm::Value *, llvm::Value *> base_pairs;
  std::set<llvm::Value *> newInsertedDefs;
  findBasePointers(result.liveset, base_pairs, &DT, DVCache, newInsertedDefs);

  if (PrintBasePointers) {
    errs() << "Base Pairs (w/o Relocation):\n";
    for (const std::pair<Value *, Value *> &I : base_pairs) {
      errs() << " derived %" << I.first->getName() << " base %"
             << I.second->getName() << "\n";
    }
  }

  result.base_pairs = base_pairs;
  result.newInsertedDefs = newInsertedDefs;
}

void SafepointPlacementImpl::fixupLiveness(
    DominatorTree &DT, const CallSite &CS,
    const std::set<Value *> &allInsertedDefs,
    PartiallyConstructedSafepointRecord &result) {
  Instruction *inst = CS.getInstruction();

  std::set<llvm::Value *> liveset = result.liveset;
  std::map<llvm::Value *, llvm::Value *> base_pairs = result.base_pairs;

  // Add bases which are used by live pointers at this safepoint.  This enables
  // a useful optimization in the loop below to avoid testing liveness, but is
  // not otherwise actually needed.  This will result in a few extra live
  // values at this safepoint (potentially).
  addBasesAsLiveValues(liveset, base_pairs);

  // For each new definition, check to see if a) the definition dominates the
  // instruction we're interested in, and b) one of the uses of that definition
  // is edge-reachable from the instruction we're interested in.  This is the
  // same definition of liveness we used in the intial liveness analysis
  for (Value *newDef : allInsertedDefs) {
    if (liveset.count(newDef)) {
      // already live, no action needed
      continue;
    }
    // PERF: Use DT to check instruction domination might not be good for
    // compilation time, and we could change to optimal solution if this
    // turn to be a issue
    if (!DT.dominates(cast<Instruction>(newDef), inst)) {
      // can't possibly be live at inst
      continue;
    }

    // PERF: This loop could be easily optimized even without moving to a true
    // liveness analysis.  Simply grouping uses by basic block would help a lot.
    for (User *User : newDef->users()) {
      Instruction *UserInst = cast<Instruction>(User);
      // Record new defs those are dominating and live at the safepoint (we
      // need to make sure def dominates safepoint since our liveness analysis
      // has this assumption)
      if (isLiveAtSafepoint(inst, UserInst, *newDef, DT, NULL)) {
        // Add the live new defs into liveset and base_pairs
        liveset.insert(newDef);
        base_pairs[newDef] = newDef;
        break; // Break out of the use loop
      }
    }
  }

  if (PrintBasePointers) {
    errs() << "Base Pairs: (w/Relocation)\n";
    for (const std::pair<Value *, Value *> &I : base_pairs) {
      errs() << " derived %" << I.first->getName() << " base %"
             << I.second->getName() << "\n";
    }
  }

  result.liveset = liveset;
  result.base_pairs = base_pairs;
}
void SafepointPlacementImpl::InsertSafepoint(
    DominatorTree &DT, const CallSite &CS, CallInst *vm_state,
    SafepointPlacementImpl::PartiallyConstructedSafepointRecord &result) {
  Instruction *inst = CS.getInstruction();
  BasicBlock *BB = inst->getParent();

  std::set<llvm::Value *> liveset = result.liveset;
  std::map<llvm::Value *, llvm::Value *> base_pairs = result.base_pairs;

  // Third, do the actual placement.
  if (!BaseRewriteOnly) {

    // Convert to vector for efficient cross referencing.
    std::vector<Value *> basevec, livevec;
    for (Value *Live : liveset) {
      livevec.push_back(Live);

      assert(base_pairs.find(Live) != base_pairs.end());
      Value *base = base_pairs[Live];
      basevec.push_back(base);
    }
    assert(livevec.size() == basevec.size());

    // To make the output IR slightly more stable (for use in diffs), ensure a
    // fixed order of the values in the safepoint (by sorting the value name).
    // The order is otherwise meaningless.
    stablize_order(basevec, livevec);

    CreateSafepoint(CS, vm_state, basevec, livevec, result);

    if (VerifyAllIR) {
      verifyFunction(*BB->getParent());
      // At this point, we've insert the new safepoint node and all of it's
      // base pointers, but we *haven't* yet performed relocation updates.
      // None of the safepoint invariants yet hold.
    }

    result.verify();

    // Note: The fact that we've inserted to the underlying instruction list
    // does not invalidate any iterators since llvm uses a doubly linked list
    // implementation internally.
  }
}

/// ---------------------------------------------
/// Everything below here is the implementation of the various phases of
/// safepoint placement.  The code should be roughly organized by phase with a
/// detailed comment describing the high level algorithm.

bool SafepointPlacementImpl::isLiveAtSafepoint(Instruction *term,
                                               Instruction *use, Value &def,
                                               DominatorTree &DT,
                                               LoopInfo *LI) {
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
void SafepointPlacementImpl::findLiveGCValuesAtInst(
    Instruction *term, BasicBlock *pred, DominatorTree &DT, LoopInfo *LI,
    std::set<llvm::Value *> &liveValues) {
  liveValues.clear();

  assert(isa<CallInst>(term) || isa<InvokeInst>(term) || term->isTerminator());

  Function *F = pred->getParent();

  // Are there any gc pointer arguments live over this point?  This needs to be
  // special cased since arguments aren't defined in basic blocks.
  for (Argument &arg : F->args()) {
    if (!isGCPointerType(arg.getType())) {
      continue;
    }

    for (User *User : arg.users()) {
      if (isLiveAtSafepoint(term, cast<Instruction>(User), arg, DT, LI)) {
        liveValues.insert(&arg);
        break;
      }
    }
  }

  // Walk through all dominating blocks - the ones which can contain
  // definitions used in this block - and check to see if any of the values
  // they define are used in locations potentially reachable from the
  // interesting instruction.
  for (Function::iterator bbi : *F) {
    BasicBlock *BBI = bbi;
    if (DT.dominates(BBI, pred)) {
      if (TraceLSP) {
        errs() << "[LSP] Looking at dominating block " << pred->getName()
               << "\n";
      }
      BUGPOINT_CLEAN_EXIT_IF(!isPotentiallyReachable(BBI, pred));
      assert(isPotentiallyReachable(BBI, pred) &&
             "dominated block must be reachable");
      // Walk through the instructions in dominating blocks and keep any
      // that have a use potentially reachable from the block we're
      // considering putting the safepoint in
      for (Instruction &inst : *BBI) {
        if (TraceLSP) {
          errs() << "[LSP] Looking at instruction ";
          inst.dump();
        }

        if (pred == BBI && (&inst) == term) {
          if (TraceLSP) {
            errs() << "[LSP] stopped because we encountered the safepoint "
                      "instruction.\n";
          }

          // If we're in the block which defines the interesting instruction,
          // we don't want to include any values as live which are defined
          // _after_ the interesting line or as part of the line itself
          // i.e. "term" is the call instruction for a call safepoint, the
          // results of the call should not be considered live in that stackmap
          break;
        }

        if (!isGCPointerType(inst.getType())) {
          if (TraceLSP) {
            errs() << "[LSP] not considering because inst not of gc pointer "
                      "type\n";
          }
          continue;
        }

        for (User *User : inst.users()) {
          Instruction *UserInst = cast<Instruction>(User);
          if (isLiveAtSafepoint(term, UserInst, inst, DT, LI)) {
            if (TraceLSP) {
              errs() << "[LSP] found live use for this safepoint ";
              UserInst->dump();
            }
            liveValues.insert(&inst);
            break;
          } else {
            if (TraceLSP) {
              errs() << "[LSP] this use does not satisfy isLiveAtSafepoint ";
              UserInst->dump();
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
Value *findBaseDefiningValue(Value *I) {
  assert(I->getType()->isPointerTy() &&
         "Illegal to ask for the base pointer of a non-pointer type");

  // There are instructions which can never return gc pointer values.  Sanity
  // check
  // that this is actually true.
  assert(!isa<InsertElementInst>(I) && !isa<ExtractElementInst>(I) &&
         !isa<ShuffleVectorInst>(I) && "Vector types are not gc pointers");
  assert((!isa<Instruction>(I) || isa<InvokeInst>(I) ||
          !cast<Instruction>(I)->isTerminator()) &&
         "With the exception of invoke terminators don't define values");
  assert(!isa<StoreInst>(I) && !isa<FenceInst>(I) &&
         "Can't be definitions to start with");
  assert(!isa<ICmpInst>(I) && !isa<FCmpInst>(I) &&
         "Comparisons don't give ops");
  // There's a bunch of instructions which just don't make sense to apply to
  // a pointer.  The only valid reason for this would be pointer bit
  // twiddling which we're just not going to support.
  assert((!isa<Instruction>(I) || !cast<Instruction>(I)->isBinaryOp()) &&
         "Binary ops on pointer values are meaningless.  Unless your "
         "bit-twiddling which we don't support");

  if (Argument *Arg = dyn_cast<Argument>(I)) {
    // An incoming argument to the function is a base pointer
    // We should have never reached here if this argument isn't an gc value
    assert(Arg->getType()->isPointerTy() &&
           "Base for pointer must be another pointer");
    return Arg;
  }

  if (GlobalVariable *global = dyn_cast<GlobalVariable>(I)) {
    // base case
    assert(AllFunctions && "should not encounter a global variable as an gc "
                           "base pointer in the VM");
    assert(global->getType()->isPointerTy() &&
           "Base for pointer must be another pointer");
    return global;
  }

  if (UndefValue *undef = dyn_cast<UndefValue>(I)) {
    // This case arises when the optimizer has recognized undefined
    // behavior.  It's also really really common when using bugpoint to
    // reduce failing cases.
    BUGPOINT_CLEAN_EXIT_IF(!AllFunctions);
    assert(AllFunctions && "should not encounter a undef base in the VM");
    assert(undef->getType()->isPointerTy() &&
           "Base for pointer must be another pointer");
    return undef; // utterly meaningless, but useful for dealing with
                  // partially optimized code.
  }

  // Due to inheritance, this must be _after_ the global variable and undef
  // checks
  if (Constant *con = dyn_cast<Constant>(I)) {
    assert(!isa<GlobalVariable>(I) && !isa<UndefValue>(I) &&
           "order of checks wrong!");
    // Note: Finding a constant base for something marked for relocation
    // doesn't really make sense.  The most likely case is either a) some
    // screwed up the address space usage or b) your validating against
    // compiled C++ code w/o the proper separation.  The only real exception
    // is a null pointer.  You could have generic code written to index of
    // off a potentially null value and have proven it null.  We also use
    // null pointers in dead paths of relocation phis (which we might later
    // want to find a base pointer for).
    assert(con->getType()->isPointerTy() &&
           "Base for pointer must be another pointer");
    assert(con->isNullValue() && "null is the only case which makes sense");
    return con;
  }

  if (CastInst *CI = dyn_cast<CastInst>(I)) {
    Value *def = CI->stripPointerCasts();
    assert(def->getType()->isPointerTy() &&
           "Base for pointer must be another pointer");
    if (isa<CastInst>(def)) {
      // If we find a cast instruction here, it means we've found a cast
      // which is not simply a pointer cast (i.e. an inttoptr).  We don't
      // know how to handle int->ptr conversion in general, but we need to
      // handle a few special cases before failing.
      IntToPtrInst *i2p = cast<IntToPtrInst>(def);
      // If the frontend marked this as a known base pointer...
      if (i2p->getMetadata("verifier_exception")) {
        return def;
      }

      // For validating against C++ hand written examples, we're just
      // going to pretend that this is a base pointer in it's own right.
      // It's a purely manufactored pointer.  This is not safe in general,
      // but is fine for manually written test cases.
      if (AllFunctions) {
        errs() << "warning: treating int as fake base: ";
        def->dump();
        return def;
      }
      // Fail hard on the general case.
      llvm_unreachable("Can not find the base pointers for an inttoptr cast");
    }
    assert(!isa<CastInst>(def) && "shouldn't find another cast here");
    return findBaseDefiningValue(def);
  }

  if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    if (LI->getType()->isPointerTy()) {
      Value *Op = LI->getOperand(0);
      // Has to be a pointer to an gc object, or possibly an array of such?
      assert(Op->getType()->isPointerTy());
      return LI; // The value loaded is an gc base itself
    }
  }
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(I)) {
    Value *Op = GEP->getOperand(0);
    if (Op->getType()->isPointerTy()) {
      return findBaseDefiningValue(Op); // The base of this GEP is the base
    }
  }

  if (AllocaInst *alloc = dyn_cast<AllocaInst>(I)) {
    // An alloca represents a conceptual stack slot.  It's the slot itself
    // that the GC needs to know about, not the value in the slot.
    assert(alloc->getType()->isPointerTy() &&
           "Base for pointer must be another pointer");
    assert(AllFunctions &&
           "should not encounter a alloca as an gc base pointer in the VM");
    return alloc;
  }

  if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(I)) {
    switch (II->getIntrinsicID()) {
    default:
      // fall through to general call handling
      break;
    case Intrinsic::gc_relocate: {
      // A previous relocation is a base case.  Examine the statepoint nodes
      // to find the gc_relocate which relocates the base of the pointer
      // we're interested in.
      CallInst *statepoint = cast<CallInst>(II->getArgOperand(0));
      ConstantInt *base_offset = cast<ConstantInt>(II->getArgOperand(1));
      for (User *User : statepoint->users()) {
        IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(User);
        // can be a gc_result use as well, we should ignore that
        if (UserInst && UserInst->getIntrinsicID() == Intrinsic::gc_relocate) {
          ConstantInt *relocated =
              cast<ConstantInt>(UserInst->getArgOperand(2));
          if (relocated == base_offset) {
            // Note: use may be II if this was a base relocation.
            return UserInst;
          }
        }
      }
      llvm_unreachable("must have relocated the base pointer");
    }
    case Intrinsic::statepoint:
      assert(false &&
             "We should never reach a statepoint in the def-use chain."
             "  That would require we're tracking the token returned.");
      break;
    case Intrinsic::gcroot:
      // Currently, this mechanism hasn't been extended to work with gcroot.
      // There's no reason it couldn't be, but I haven't thought about the
      // implications much.
      assert(false && "Not yet handled");
    }
  }
  // Let's assume that any call we see is to a java function.  Java
  // functions can only return Java objects (i.e. base pointers).
  // Note: when we add runtime functions which return non-base pointers we
  // will need to revisit this.  (Will this ever happen?)
  if (CallInst *call = dyn_cast<CallInst>(I)) {
    assert(call->getType()->isPointerTy() &&
           "Base for pointer must be another pointer");
    return call;
  }
  if (InvokeInst *invoke = dyn_cast<InvokeInst>(I)) {
    assert(invoke->getType()->isPointerTy() &&
           "Base for pointer must be another pointer");
    return invoke;
  }

  // I have absolutely no idea how to implement this part yet.  It's not
  // neccessarily hard, I just haven't really looked at it yet.
  assert(!isa<LandingPadInst>(I) && "Landing Pad is unimplemented");

  if (AtomicCmpXchgInst *cas = dyn_cast<AtomicCmpXchgInst>(I)) {
    // A CAS is effectively a atomic store and load combined under a
    // predicate.  From the perspective of base pointers, we just treat it
    // like a load.  We loaded a pointer from a address in memory, that value
    // had better be a valid base pointer.
    return cas->getPointerOperand();
  }
  if (AtomicRMWInst *atomic = dyn_cast<AtomicRMWInst>(I)) {
    assert(AtomicRMWInst::Xchg == atomic->getOperation() &&
           "All others are binary ops which don't apply to base pointers");
    // semantically, a load, store pair.  Treat it the same as a standard load
    return atomic->getPointerOperand();
  }

  // The aggregate ops.  Aggregates can either be in the heap or on the
  // stack, but in either case, this is simply a field load.  As a result,
  // this is a defining definition of the base just like a load is.
  if (ExtractValueInst *ev = dyn_cast<ExtractValueInst>(I)) {
    return ev;
  }

  // We should never see an insert vector since that would require we be
  // tracing back a struct value not a pointer value.
  assert(!isa<InsertValueInst>(I) &&
         "Base pointer for a struct is meaningless");

  // The last two cases here don't return a base pointer.  Instead, they
  // return a value which dynamically selects from amoung several base
  // derived pointers (each with it's own base potentially).  It's the job of
  // the caller to resolve these.
  if (SelectInst *select = dyn_cast<SelectInst>(I)) {
    return select;
  }
  if (PHINode *phi = dyn_cast<PHINode>(I)) {
    return phi;
  }

  errs() << "unknown type: ";
  I->dump();
  assert(false && "unknown type");
  return NULL;
}

/// Returns the base defining value for this value.
Value *findBaseDefiningValueCached(Value *I, DefiningValueMapTy &cache) {
  if (cache.find(I) == cache.end()) {
    cache[I] = findBaseDefiningValue(I);
  }
  assert(cache.find(I) != cache.end());

  if (TraceLSP) {
    errs() << "fBDV-cached: " << I->getName() << " -> " << cache[I]->getName()
           << "\n";
  }
  return cache[I];
}

/// Return a base pointer for this value if known.  Otherwise, return it's
/// base defining value.
Value *findBaseOrBDV(Value *I, DefiningValueMapTy &cache) {
  Value *def = findBaseDefiningValueCached(I, cache);
  if (cache.count(def)) {
    // Either a base-of relation, or a self reference.  Caller must check.
    return cache[def];
  }
  // Only a BDV available
  return def;
}

Value *findRelocateValueAtSP(Instruction *statepoint, Value *def) {
  for (User *User : statepoint->users()) {
    IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(User);
    // can be a gc_result use as well, we should ignore that
    if (UserInst && UserInst->getIntrinsicID() == Intrinsic::gc_relocate) {
      GCRelocateOperands relocate(UserInst);
      if (def == relocate.derivedPtr()) {
        return UserInst;
      }
    }
  }
  return NULL;
}

/// Given the result of a call to findBaseDefiningValue, or findBaseOrBDV,
/// is it known to be a base pointer?  Or do we need to continue searching.
bool isKnownBaseResult(Value *v) {
  if (!isa<PHINode>(v) && !isa<SelectInst>(v)) {
    // no recursion possible
    return true;
  }
  if (cast<Instruction>(v)->getMetadata("is_base_value")) {
    // This is a previously inserted base phi or select.  We know
    // that this is a base value.
    return true;
  }

  // We need to keep searching
  return false;
}

// TODO: find a better name for this
class PhiState {
public:
  enum Status {
    Unknown,
    Base,
    Conflict
  };

  PhiState(Status s, Value *b = NULL) : status(s), base(b) {
    assert(status != Base || b);
  }
  PhiState(Value *b) : status(Base), base(b) {}
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

  bool operator!=(const PhiState &other) const { return !(*this == other); }

  void dump() {
    errs() << status << " (" << base << " - "
           << (base ? base->getName() : "NULL") << "): ";
  }

private:
  Status status;
  Value *base; // non null only if status == base
};

// Values of type PhiState form a lattice, and this is a helper
// class that implementes the meet operation.  The meat of the meet
// operation is implemented in MeetPhiStates::pureMeet
class MeetPhiStates {
public:
  // phiStates is a mapping from PHINodes and SelectInst's to PhiStates.
  explicit MeetPhiStates(const std::map<Value *, PhiState> &phiStates)
      : phiStates(phiStates) {}

  // Destructively meet the current result with the base V.  V can
  // either be a merge instruction (SelectInst / PHINode), in which
  // case its status is looked up in the phiStates map; or a regular
  // SSA value, in which case it is assumed to be a base.
  void meetWith(Value *V) {
    PhiState otherState = getStateForBDV(V);
    assert((MeetPhiStates::pureMeet(otherState, currentResult) ==
            MeetPhiStates::pureMeet(currentResult, otherState)) &&
           "math is wrong: meet does not commute!");
    currentResult = MeetPhiStates::pureMeet(otherState, currentResult);
  }

  PhiState getResult() const { return currentResult; }

private:
  const std::map<Value *, PhiState> &phiStates;
  PhiState currentResult;

  /// Return a phi state for a base defining value.  We'll generate a new
  /// base state for known bases and expect to find a cached state otherwise
  PhiState getStateForBDV(Value *baseValue) {
    if (isKnownBaseResult(baseValue)) {
      return PhiState(baseValue);
    } else {
      return lookupFromMap(baseValue);
    }
  }

  PhiState lookupFromMap(Value *V) {
    map<Value *, PhiState>::const_iterator I = phiStates.find(V);
    assert(I != phiStates.end() && "lookup failed!");
    return I->second;
  }

  static PhiState pureMeet(const PhiState &stateA, const PhiState &stateB) {
    switch (stateA.getStatus()) {
    case PhiState::Unknown:
      return stateB;

    case PhiState::Base:
      assert(stateA.getBase() && "can't be null");
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
  }
};

/// For a given value or instruction, figure out what base ptr it's derived
/// from.  For gc objects, this is simply itself.  On success, returns a value
/// which is the base pointer.  (This is reliable and can be used for
/// relocation.)  On failure, returns NULL.
Value *findBasePointer(Value *I, DefiningValueMapTy &cache,
                       std::set<llvm::Value *> &newInsertedDefs) {
  Value *def = findBaseOrBDV(I, cache);

  if (isKnownBaseResult(def)) {
    return def;
  }

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

  map<Value *, PhiState> states;
  states[def] = PhiState();
  // Recursively fill in all phis & selects reachable from the initial one
  // for which we don't already know a definite base value for
  // PERF: Yes, this is as horribly inefficient as it looks.
  bool done = false;
  while (!done) {
    done = true;
    for (const std::pair<Value *, PhiState> &I : states) {
      Value *v = I.first;
      assert(!isKnownBaseResult(v) && "why did it get added?");
      if (PHINode *phi = dyn_cast<PHINode>(v)) {
        assert(phi->getNumIncomingValues() > 0 &&
               "zero input phis are illegal");
        for (Value *InVal : phi->operands()) {
          Value *local = findBaseOrBDV(InVal, cache);
          if (!isKnownBaseResult(local) && states.find(local) == states.end()) {
            states[local] = PhiState();
            done = false;
          }
        }
      } else if (SelectInst *sel = dyn_cast<SelectInst>(v)) {
        Value *local = findBaseOrBDV(sel->getTrueValue(), cache);
        if (!isKnownBaseResult(local) && states.find(local) == states.end()) {
          states[local] = PhiState();
          done = false;
        }
        local = findBaseOrBDV(sel->getFalseValue(), cache);
        if (!isKnownBaseResult(local) && states.find(local) == states.end()) {
          states[local] = PhiState();
          done = false;
        }
      }
    }
  }

  if (TraceLSP) {
    errs() << "States after initialization:\n";
    for (const std::pair<Value *, PhiState> &I : states) {
      Instruction *v = cast<Instruction>(I.first);
      PhiState state = I.second;
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
    for (const std::pair<Value *, PhiState> &I : states) {
      MeetPhiStates calculateMeet(states);
      Value *v = I.first;
      assert(!isKnownBaseResult(v) && "why did it get added?");
      assert(isa<SelectInst>(v) || isa<PHINode>(v));
      if (SelectInst *select = dyn_cast<SelectInst>(v)) {
        calculateMeet.meetWith(findBaseOrBDV(select->getTrueValue(), cache));
        calculateMeet.meetWith(findBaseOrBDV(select->getFalseValue(), cache));
      } else if (PHINode *phi = dyn_cast<PHINode>(v)) {
        for (Value *InVal : phi->operands()) {
          calculateMeet.meetWith(findBaseOrBDV(InVal, cache));
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

  if (TraceLSP) {
    errs() << "States after meet iteration:\n";
    for (const std::pair<Value *, PhiState> &I : states) {
      Instruction *v = cast<Instruction>(I.first);
      PhiState state = I.second;
      state.dump();
      v->dump();
    }
  }

  // Insert Phis for all conflicts
  for (const std::pair<Value *, PhiState> &I : states) {
    Instruction *v = cast<Instruction>(I.first);
    PhiState state = I.second;
    assert(!isKnownBaseResult(v) && "why did it get added?");
    assert(!state.isUnknown() && "Optimistic algorithm didn't complete!");
    if (state.isConflict()) {
      if (isa<PHINode>(v)) {
        int num_preds =
            std::distance(pred_begin(v->getParent()), pred_end(v->getParent()));
        assert(num_preds > 0 && "how did we reach here");
        PHINode *phi = PHINode::Create(v->getType(), num_preds, "base_phi", v);
        newInsertedDefs.insert(phi);
        // Add metadata marking this as a base value
        Value *const_1 = ConstantInt::get(
            Type::getInt32Ty(
                v->getParent()->getParent()->getParent()->getContext()),
            1);
        MDNode *md = MDNode::get(
            v->getParent()->getParent()->getParent()->getContext(), const_1);
        phi->setMetadata("is_base_value", md);
        states[v] = PhiState(PhiState::Conflict, phi);
      } else if (SelectInst *sel = dyn_cast<SelectInst>(v)) {
        // The undef will be replaced later
        UndefValue *undef = UndefValue::get(sel->getType());
        SelectInst *basesel = SelectInst::Create(sel->getCondition(), undef,
                                                 undef, "base_select", sel);
        newInsertedDefs.insert(basesel);
        // Add metadata marking this as a base value
        Value *const_1 = ConstantInt::get(
            Type::getInt32Ty(
                v->getParent()->getParent()->getParent()->getContext()),
            1);
        MDNode *md = MDNode::get(
            v->getParent()->getParent()->getParent()->getContext(), const_1);
        basesel->setMetadata("is_base_value", md);
        states[v] = PhiState(PhiState::Conflict, basesel);
      } else {
        assert(false);
      }
    }
  }

  // Fixup all the inputs of the new PHIs
  for (const std::pair<Value *, PhiState> &I : states) {
    Instruction *v = cast<Instruction>(I.first);
    PhiState state = I.second;

    assert(!isKnownBaseResult(v) && "why did it get added?");
    assert(!state.isUnknown() && "Optimistic algorithm didn't complete!");
    if (state.isConflict()) {
      if (PHINode *basephi = dyn_cast<PHINode>(state.getBase())) {
        PHINode *phi = cast<PHINode>(v);
        unsigned NumPHIValues = phi->getNumIncomingValues();
        for (unsigned i = 0; i < NumPHIValues; i++) {
          Value *InVal = phi->getIncomingValue(i);
          BasicBlock *InBB = phi->getIncomingBlock(i);
          // Find either the defining value for the PHI or the normal base for
          // a non-phi node
          Value *base = findBaseOrBDV(InVal, cache);
          if (!isKnownBaseResult(base)) {
            // Either conflict or base.
            assert(states.count(base));
            base = states[base].getBase();
            assert(base != NULL && "unknown PhiState!");
          }
          assert(base && "can't be null");
          // Must use original input BB since base may not be Instruction
          // The cast is needed since base traversal may strip away bitcasts
          if (base->getType() != basephi->getType()) {
            base = new BitCastInst(base, basephi->getType(), "cast",
                                   InBB->getTerminator());
          }
          basephi->addIncoming(base, InBB);
        }
        assert(basephi->getNumIncomingValues() == NumPHIValues);
      } else if (SelectInst *basesel = dyn_cast<SelectInst>(state.getBase())) {
        SelectInst *sel = cast<SelectInst>(v);
        // Operand 1 & 2 are true, false path respectively. TODO: refactor to
        // something more safe and less hacky.
        for (int i = 1; i <= 2; i++) {
          Value *InVal = sel->getOperand(i);
          // Find either the defining value for the PHI or the normal base for
          // a non-phi node
          Value *base = findBaseOrBDV(InVal, cache);
          if (!isKnownBaseResult(base)) {
            // Either conflict or base.
            assert(states.count(base));
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

  // Cache all of our results so we can cheaply reuse them
  // NOTE: This is actually two caches: one of the base defining value
  // relation and one of the base pointer relation!  FIXME
  for (const auto &item : states) {
    Value *v = item.first;
    Value *base = item.second.getBase();
    assert(v && base);
    assert(!isKnownBaseResult(v) && "why did it get added?");

    if (TraceLSP) {
      string fromstr = cache.count(v)
                           ? (cache[v]->hasName() ? cache[v]->getName() : "")
                           : "none";
      errs() << "Updating base value cache"
             << " for: " << (v->hasName() ? v->getName() : "")
             << " from: " << fromstr
             << " to: " << (base->hasName() ? base->getName() : "") << "\n";
    }

    assert(isKnownBaseResult(base) &&
           "must be something we 'know' is a base pointer");
    if (cache.count(v)) {
      // Once we transition from the BDV relation being store in the cache to
      // the base relation being stored, it must be stable
      assert((!isKnownBaseResult(cache[v]) || cache[v] == base) &&
             "base relation should be stable");
    }
    cache[v] = base;
  }
  assert(cache.find(def) != cache.end());
  return cache[def];
}
}
/// For a set of live pointers (base and/or derived), identify the base
/// pointer of the object which they are derived from.
void SafepointPlacementImpl::findBasePointers(
    const std::set<llvm::Value *> &live,
    std::map<llvm::Value *, llvm::Value *> &base_pairs, DominatorTree *DT,
    DefiningValueMapTy &DVCache, std::set<llvm::Value *> &newInsertedDefs) {
  for (Value *ptr : live) {
    Value *base = findBasePointer(ptr, DVCache, newInsertedDefs);
    assert(base && "failed to find base pointer");
    base_pairs[ptr] = base;
    assert((!isa<Instruction>(base) || !isa<Instruction>(ptr) ||
            DT->dominates(cast<Instruction>(base)->getParent(),
                          cast<Instruction>(ptr)->getParent())) &&
           "The base we found better dominate the derived pointer");
  }
}

CallInst *SafepointPlacementImpl::findVMState(llvm::Instruction *term,
                                              DominatorTree *DT) {
  // At this time, we look for a vmstate call dominating 'term'
  // By construction, if there was one in the original IR generated by the
  // frontend, a valid one is still available.

  BasicBlock::reverse_iterator I = term->getParent()->rbegin(),
                               E = term->getParent()->rend();
  // Find the BasicBlock::reverse_iterator pointing to term.  Is there a direct
  // way to do this?
  while (&*I != term && I != E)
    I++;
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
    if (!DT->getNode(I->getParent())->getIDom())
      break; // and crash
    BasicBlock *immediateDominator =
        DT->getNode(I->getParent())->getIDom()->getBlock();
    BUGPOINT_CLEAN_EXIT_IF(!immediateDominator);
    if (immediateDominator == NULL)
      break; // and crash!

    I = immediateDominator->rbegin();
    E = immediateDominator->rend();
  }

  return NULL;
}

namespace {

void
VerifySafepointBounds(const std::pair<Instruction *, Instruction *> &bounds) {
  assert(bounds.first->getParent() && bounds.second->getParent() &&
         "both must belong to basic blocks");
  if (bounds.first->getParent() == bounds.second->getParent()) {
    // This is a call safepoint
    // TODO: scan the range to find the statepoint
    // TODO: check that the following instruction is not a gc_relocate or
    // gc_result
  } else {
    // This is an invoke safepoint
    BasicBlock *BB = bounds.first->getParent();
    InvokeInst *invoke = dyn_cast<InvokeInst>(BB->getTerminator());
    assert(invoke && "only continues over invokes!");
    assert(invoke->getNormalDest() == bounds.second->getParent() &&
           "safepoint can only continue into normal exit block");
  }
}

int find_index(const std::vector<Value *> &livevec, Value *val) {
  std::vector<Value *>::const_iterator itr =
      std::find(livevec.begin(), livevec.end(), val);
  assert(livevec.end() != itr);
  size_t index = std::distance(livevec.begin(), itr);
  assert(index < livevec.size());
  return index;
}
}

/// Inserts the actual code for a safepoint.  WARNING: Does not do any fixup
/// to adjust users of the original live values.  That's the callers
/// responsibility.
void SafepointPlacementImpl::CreateSafepoint(
    const CallSite &CS, /* to replace */
    llvm::CallInst *jvmStateCall, const std::vector<llvm::Value *> &basePtrs,
    const std::vector<llvm::Value *> &liveVariables,
    SafepointPlacementImpl::PartiallyConstructedSafepointRecord &result) {
  assert(basePtrs.size() == liveVariables.size());

  BasicBlock *BB = CS.getInstruction()->getParent();
  assert(BB);
  Function *F = BB->getParent();
  assert(F && "must be set");
  Module *M = F->getParent();
  assert(M && "must be set");

  // TODO: technically, a pass is not allowed to get functions from within a
  // function pass since it might trigger a new function addition.  Refactor
  // this logic out to the initialization of the pass.  Doesn't appear to
  // matter in practice.

  // Fill in the one generic type'd argument (the function is also vararg)
  std::vector<Type *> argTypes;
  argTypes.push_back(CS.getCalledValue()->getType());
  Function *gc_statepoint_decl =
      Intrinsic::getDeclaration(M, Intrinsic::statepoint, argTypes);

  // Then go ahead and use the builder do actually do the inserts.  We insert
  // immediately before the previous instruction under the assumption that all
  // arguments will be available here.  We can't insert afterwards since we may
  // be replacing a terminator.
  Instruction *insertBefore = CS.getInstruction();
  IRBuilder<> Builder(insertBefore);
  // First, create the statepoint (with all live ptrs as arguments).
  std::vector<llvm::Value *> args;
  // target, #args, flags, bci, #stack, #locals, #monitors
  Value *Target = CS.getCalledValue();
  args.push_back(Target);
  args.push_back(
      ConstantInt::get(Type::getInt32Ty(M->getContext()), CS.arg_size()));
  args.push_back(
      ConstantInt::get(Type::getInt32Ty(M->getContext()), 0 /*unused*/));

  IntegerType *i32Ty = Type::getInt32Ty(M->getContext());

  if (jvmStateCall) {
    // Bugpoint doesn't know these are special and tries to remove arguments
    BUGPOINT_CLEAN_EXIT_IF(jvmStateCall->getNumArgOperands() < 4);

    JVMState jvmState(jvmStateCall);

    args.push_back(ConstantInt::get(i32Ty, jvmState.bci()));
    args.push_back(ConstantInt::get(i32Ty, jvmState.numStackElements()));
    args.push_back(ConstantInt::get(i32Ty, jvmState.numLocals()));
    args.push_back(ConstantInt::get(i32Ty, jvmState.numMonitors()));
  } else {
    // All of these are placeholders until we actually grab the deopt state
    args.push_back(ConstantInt::get(i32Ty, -1));
    args.push_back(ConstantInt::get(i32Ty, 0));
    args.push_back(ConstantInt::get(i32Ty, 0));
    args.push_back(ConstantInt::get(i32Ty, 0));
  }

  // Copy all the arguments of the original call
  args.insert(args.end(), CS.arg_begin(), CS.arg_end());

  if (jvmStateCall) {
    JVMState jvmState(jvmStateCall);

    for (int i = 0; i < jvmState.numStackElements(); i++) {
      args.push_back(ConstantInt::get(
          i32Ty, jvmState.stackElementTypeAt(i).coerceToInt()));
      args.push_back(jvmState.stackElementAt(i));
    }

    for (int i = 0; i < jvmState.numLocals(); i++) {
      args.push_back(
          ConstantInt::get(i32Ty, jvmState.localTypeAt(i).coerceToInt()));
      args.push_back(jvmState.localAt(i));
    }

    for (int i = 0; i < jvmState.numMonitors(); i++) {
      args.push_back(jvmState.monitorAt(i));
    }
  }

  // add all the pointers to be relocated (gc arguments)
  // Capture the start of the live variable list for use in the gc_relocates
  const int live_start = args.size();
  args.insert(args.end(), liveVariables.begin(), liveVariables.end());

  // Create the statepoint given all the arguments
  Instruction *token = NULL;
  if (CS.isCall()) {
    CallInst *toReplace = cast<CallInst>(CS.getInstruction());
    CallInst *call =
        Builder.CreateCall(gc_statepoint_decl, args, "safepoint_token");
    call->setTailCall(toReplace->isTailCall());
    call->setCallingConv(toReplace->getCallingConv());
    // I believe this copies both param and function attributes - TODO: test
    call->setAttributes(toReplace->getAttributes());
    token = call;

    // Put the following gc_result and gc_relocate calls immediately after the
    // the old call (which we're about to delete)
    BasicBlock::iterator next(toReplace);
    assert(BB->end() != next && "not a terminator, must have next");
    next++;
    Instruction *IP = &*(next);
    Builder.SetInsertPoint(IP);
    Builder.SetCurrentDebugLocation(IP->getDebugLoc());

  } else if (CS.isInvoke()) {
    InvokeInst *toReplace = cast<InvokeInst>(CS.getInstruction());

    // Insert a new basic block which will become the normal destination of our
    // modified invoke.  This is needed since the original normal destinition
    // can potentially be reachable along other paths.
    BasicBlock *normalDest =
        BasicBlock::Create(M->getContext(), "invoke_safepoint_normal_dest", F,
                           toReplace->getNormalDest());
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
    InvokeInst *invoke = Builder.CreateInvoke(gc_statepoint_decl, normalDest,
                                              toReplace->getUnwindDest(), args);
    invoke->setCallingConv(toReplace->getCallingConv());
    // I believe this copies both param and function attributes - TODO: test
    invoke->setAttributes(toReplace->getAttributes());
    token = invoke;

    // Put all the gc_result and gc_return value calls into the normal control
    // flow block
    Instruction *IP = &*(normalDest->getFirstInsertionPt());
    Builder.SetInsertPoint(IP);
    Builder.SetCurrentDebugLocation(toReplace->getDebugLoc());
  } else {
    llvm_unreachable("unexpect type of CallSite");
  }
  assert(token);

  // Handle the return value of the original call - update all uses to use a
  // gc_result hanging off the statepoint node we just inserted

  // Only add the gc_result iff there is actually a used result
  Instruction *gc_result = NULL;
  if (!CS.getType()->isVoidTy() && !CS.getInstruction()->use_empty()) {
    vector<Type *> types;          // one per 'any' type
    types.push_back(CS.getType()); // result type
    Value *gc_result_func = NULL;
    if (CS.getType()->isIntegerTy()) {
      gc_result_func =
          Intrinsic::getDeclaration(M, Intrinsic::gc_result_int, types);
    } else if (CS.getType()->isFloatingPointTy()) {
      gc_result_func =
          Intrinsic::getDeclaration(M, Intrinsic::gc_result_float, types);
    } else if (CS.getType()->isPointerTy()) {
      gc_result_func =
          Intrinsic::getDeclaration(M, Intrinsic::gc_result_ptr, types);
    } else {
      llvm_unreachable("non java type encountered");
    }

    vector<Value *> args;
    args.push_back(token);
    gc_result = Builder.CreateCall(
        gc_result_func, args,
        CS.getInstruction()->hasName() ? CS.getInstruction()->getName() : "");
  }
  result.result = gc_result;

  // Second, create a gc.relocate for every live variable
  std::vector<llvm::Instruction *> newDefs;
  for (unsigned i = 0; i < liveVariables.size(); i++) {
    // We generate a (potentially) unique declaration for every pointer type
    // combination.  This results is some blow up the function declarations in
    // the IR, but removes the need for argument bitcasts which shrinks the IR
    // greatly and makes it much more readable.
    vector<Type *> types;                         // one per 'any' type
    types.push_back(liveVariables[i]->getType()); // result type
    Value *gc_relocate_decl =
        Intrinsic::getDeclaration(M, Intrinsic::gc_relocate, types);

    // Generate the gc.relocate call and save the result
    vector<Value *> args;
    args.push_back(token);
    args.push_back(
        ConstantInt::get(Type::getInt32Ty(M->getContext()),
                         live_start + find_index(liveVariables, basePtrs[i])));
    args.push_back(ConstantInt::get(
        Type::getInt32Ty(M->getContext()),
        live_start + find_index(liveVariables, liveVariables[i])));
    // only specify a debug name if we can give a useful one
    Value *reloc = Builder.CreateCall(
        gc_relocate_decl, args, liveVariables[i]->hasName()
                                    ? liveVariables[i]->getName() + ".relocated"
                                    : "");
    // Trick CodeGen into thinking there are lots of free registers at this
    // fake call.
    cast<CallInst>(reloc)->setCallingConv(CallingConv::Cold);

    newDefs.push_back(cast<Instruction>(reloc));
  }
  assert(newDefs.size() == liveVariables.size() &&
         "missing or extra redefinition at safepoint");

  // PERF: Using vectors where array literals and reserves would be better.

  // Need to pass through the last part of the safepoint block so that we
  // don't accidentally update uses in a following gc.relocate which is
  // still conceptually part of the same safepoint.  Gah.
  Instruction *last = NULL;
  if (!newDefs.empty()) {
    last = newDefs.back();
  } else if (gc_result) {
    last = gc_result;
  } else {
    last = token;
  }
  assert(last && "can't be null");
  const std::pair<Instruction *, Instruction *> bounds = make_pair(token, last);

  // Sanity check our results - this is slightly non-trivial due to invokes
  VerifySafepointBounds(bounds);

  result.safepoint = bounds;
}

namespace {
bool atLeastOnePhiInBB(const set<PHINode *> &phis, const BasicBlock *BB) {
  for (PHINode *Phi : phis) {
    if (Phi->getParent() == BB) {
      return true;
    }
  }
  return false;
}

void updatePHIUses(DominatorTree &DT, Value *oldDef,
                   std::map<BasicBlock *, Value *> &seen,
                   set<PHINode *> &newPHIs, bool isNewPhi) {
  for (PHINode *phi : newPHIs) {
    BasicBlock *BB = phi->getParent();
    if (!isNewPhi) {
      unsigned NumPHIValues = phi->getNumIncomingValues();
      assert(NumPHIValues > 0 && "zero input phis are illegal");
      assert(NumPHIValues == std::distance(pred_begin(BB), pred_end(BB)) &&
             "wrong number of inputs");
    }

    assert(pred_begin(BB) != pred_end(BB) &&
           "shouldn't reach block without predecessors");
    // We could have several same incoming basicblocks into the phi node (think
    // about a switch case),
    // and they will carry the same value across the edge as long as their
    // incoming and destination
    // block are the same, and won't break our flow analysis.

    // this will walk through all predecessors (and could have duplicated
    // basicblocks)
    for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; PI++) {
      BasicBlock *pred = *PI;
      Value *def = NULL;
      if (seen.find(pred) != seen.end() && NULL != seen[pred]) {
        def = seen[pred];
        // Note: seen[pred] may actual dominate phi.  In particular,
        // backedges of loops with a def in the preheader make this really
        // common.  The phi is still needed.
        //          assert( isPotentiallyReachable(seen[pred]->getParent(), BB,
        // &DT) && "sanity check - provably reachable by alg above.");
      } else if (seen.find(pred) != seen.end() && NULL == seen[pred]) {
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
        // 2) if we're wrong about this being unused, we'll hopefully fault
        // cleanly.
        def = ConstantPointerNull::get(cast<PointerType>(phi->getType()));
      } else {
        assert(seen.find(pred) == seen.end() &&
               "kill's should have been handled above");
        // This must be coming from an unreachable block
        def = ConstantPointerNull::get(cast<PointerType>(phi->getType()));
        // Can't assert unreachable since routine is conservative about
        // reachability
      }
      assert(def && "must have found a def");
      if (isNewPhi) {
        phi->addIncoming(def, pred);
      } else {
        // update all incoming values related to this predecessor
        for (size_t i = 0; i < phi->getNumIncomingValues(); i++) {
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

          if (phi->getIncomingBlock(i) == pred &&
              phi->getIncomingValue(i) == oldDef) {
            phi->setIncomingValue(i, def);
          }
        }
      }
    }
    unsigned NumPHIValues = phi->getNumIncomingValues();
    assert(NumPHIValues > 0 && "zero input phis are illegal");
    assert(NumPHIValues == std::distance(pred_begin(BB), pred_end(BB)) &&
           "wrong number of inputs");
  }
}
}

void SafepointPlacementImpl::relocationViaAlloca(
    Function &F, DominatorTree &DT, const std::vector<Value *> &live,
    const std::vector<struct PartiallyConstructedSafepointRecord> &records) {
#ifndef NDEBUG
  int initialAllocaNum = 0;

  // record initial number of allocas
  for (const BasicBlock &BB : F) {
    for (const Instruction &Inst : BB) {
      if (isa<AllocaInst>(Inst))
        initialAllocaNum++;
    }
  }
#endif

  std::map<Value *, Value *> allocaMap;
  std::vector<AllocaInst *> PromotableAllocas;

  // emit alloca for each live gc pointer
  for (Value *liveValue : live) {
    AllocaInst *alloca = new AllocaInst(liveValue->getType(), "",
                                        F.getEntryBlock().getFirstNonPHI());
    allocaMap[liveValue] = alloca;
    PromotableAllocas.push_back(alloca);
  }

  // update use with load allocas and add store for gc_relocated
  for (const std::pair<Value *, Value *> &I : allocaMap) {
    Value *def = I.first;
    Value *alloca = I.second;

    // update gc pointer after each statepoint
    // either store a relocated value or null (if no relocated value found for
    // this gc pointer and it is not a gc_result)
    // this must happen before we update the statepoint with load of alloca
    // otherwise we lose the link between statepoint and old def
    for (const struct PartiallyConstructedSafepointRecord &info : records) {
      Value *relocatedValue = findRelocateValueAtSP(info.safepoint.first, def);
      if (relocatedValue != NULL) {
        StoreInst *store = new StoreInst(relocatedValue, alloca);
        store->insertAfter(cast<Instruction>(relocatedValue));
      } else if (def != info.result) {
        StoreInst *store = new StoreInst(
            ConstantPointerNull::get(cast<PointerType>(def->getType())),
            alloca);
        store->insertAfter(info.safepoint.second);
      }
    }

    // we pre-record the uses of allocas so that we dont have to worry about
    // later update
    // that change the user information.
    std::vector<Instruction *> users;

    for (User *User : def->users()) {
      users.push_back(dyn_cast<Instruction>(User));
    }

    unique_unsorted(users);

    for (Instruction *User : users) {
      if (isa<PHINode>(User)) {
        PHINode *phi = cast<PHINode>(User);
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
          if (def == phi->getIncomingValue(i)) {
            LoadInst *load = new LoadInst(
                alloca, "", phi->getIncomingBlock(i)->getTerminator());
            phi->setIncomingValue(i, load);
          }
        }
      } else {
        LoadInst *load = new LoadInst(alloca, "", User);
        User->replaceUsesOfWith(def, load);
      }
    }

    // emit store for the initial gc value
    // store must be inserted after load, otherwise store will be in alloca's
    // use list and an extra load will be inserted before it
    StoreInst *store = new StoreInst(def, alloca);
    if (isa<Instruction>(def)) {
      store->insertAfter(cast<Instruction>(def));
    } else {
      assert((isa<Argument>(def) || isa<GlobalVariable>(def)) &&
             "Must be argument or global");
      store->insertAfter(cast<Instruction>(alloca));
    }
  }

  assert(PromotableAllocas.size() == live.size() &&
         "we must have the same allocas with lives");
  if (!PromotableAllocas.empty()) {
    // apply mem2reg to promote alloca to SSA
    PromoteMemToReg(PromotableAllocas, DT);
  }

#ifndef NDEBUG
  for (const BasicBlock &BB : F) {
    for (const Instruction &Inst : BB) {
      if (isa<AllocaInst>(Inst))
        initialAllocaNum--;
    }
  }
  assert(initialAllocaNum == 0 && "We must not introduce any extra allocas");
#endif
}

/* Note: We settled on using a 'simple' data flow algorithm after experimenting
   with a dominance based alogrithm.  This can and should be improved, but the
   inductive invariants turned out to be suprisingly complicated and bugprone.
*/
void SafepointPlacementImpl::insertPHIsForNewDef(DominatorTree &DT, Function &F,
                                                 Value *oldDef) {

  // PERF: most of this could be done for all the updated values at once.  It
  // should be fairly easy to rewrite this to work on vectors of old and new
  // defs.

  if (TraceLSP) {
    errs() << "Relocating %" << oldDef->getName() << "\n";
  }

  // block to dominating def _on exit_
  std::map<BasicBlock *, Value *> seen;
  // The basic block to be visited and the last dominating def _on entry_ of
  // that block
  typedef std::pair<BasicBlock *, Value *> frontier_node;
  std::vector<frontier_node> frontier;
  std::set<PHINode *> phis, newPHIs;

  // Unconditionally add all phi uses to be updated.  This will include some
  // outside of the reachable region, but we won't actually update those (since
  // the input edges will never be explored).  This allows us to terminate the
  // search when leaving the region dominated by oldDef without worrying about
  // phis in the blocks reachable immediately outside that region.
  for (User *User : oldDef->users()) {
    if (PHINode *phi = dyn_cast<PHINode>(User)) {
      phis.insert(phi);
    }
  }

  if (isa<Argument>(oldDef)) {
    // Push the entry block (where the argument is valid on entry)
    frontier.push_back(make_pair(&F.getEntryBlock(), oldDef));
  } else {
    // Push the entry block (which has no valid def for old def)
    frontier.push_back(make_pair(&F.getEntryBlock(), (Value *)NULL));
  }
  while (!frontier.empty()) {
    const frontier_node current = frontier.back();
    frontier.pop_back();

    BasicBlock *currentBB = current.first;
    Value *currentDef = current.second;

    if (TraceLSP) {
      errs() << "[TraceRelocations] entering %" << currentBB->getName()
             << " with value %" << (currentDef ? currentDef->getName() : "NULL")
             << "\n";
    }

    assert(currentBB && "Can't be null");
    assert((!currentDef || currentDef->getType() == oldDef->getType()) &&
           "Types of definitions must match");
    if (seen.find(currentBB) != seen.end()) {
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
    // | \  -- suppress multiline comment warning
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
    assert(!atLeastOnePhiInBB(newPHIs, currentBB) &&
           "If it already has a phi, why are we here?");

    Value *exitDef = NULL;

    int num_preds = std::distance(pred_begin(currentBB), pred_end(currentBB));
    // The most trivial possible condition
    if (num_preds > 1) {

      // Sanity check the phis we encounter
      for (BasicBlock::iterator itr = currentBB->begin(),
                                end = currentBB->getFirstNonPHI();
           itr != end; itr++) {
        PHINode *phi = cast<PHINode>(&*itr);
        assert(phi->getNumIncomingValues() > 0 &&
               "zero input phis are illegal");
        for (Value *InVal : phi->operands()) {
          if (InVal == oldDef) {
            if (phi->getMetadata("is_relocation_phi")) {
              // We've already seen this block, how'd we reach it again?
              llvm_unreachable("relocation phi encountered!");
            }
            // We'll need to make sure that this use gets updated properly.
            // Note that the phi is live _even if_ the basic block is not due
            // to a relocation phi
            assert(phis.find(phi) != phis.end() &&
                   "must already be a use of oldDef");

            break;
          }
        }
      }

      // There's two parts of PHI insertion:
      // 1) Constructing the actual phi node
      // 2) Rewriting uses to use the new definition
      // 3) Figuring out what inputs the new phi has
      // We know enough to do 1 & 2, but not yet 3.  We'll have to come
      // back to that after finding all insert sites
      int num_preds = std::distance(pred_begin(currentBB), pred_end(currentBB));
      PHINode *phi = PHINode::Create(oldDef->getType(), num_preds, "relocated",
                                     &currentBB->front());
      // Add a metadata annotation marking this as a relocation phi
      Value *const_1 =
          ConstantInt::get(Type::getInt32Ty(F.getParent()->getContext()), 1);
      MDNode *md = MDNode::get(F.getParent()->getContext(), const_1);
      phi->setMetadata("is_relocation_phi", md);
      newPHIs.insert(phi);
      exitDef = phi;
    } else {
      exitDef = currentDef;
    }
    for (BasicBlock::iterator itr = currentBB->begin(),
                              end = currentBB->getFirstNonPHI();
         itr != end; itr++) {
      Instruction *inst = &*itr;

      if (inst == oldDef) {
        // We encountered the original definition
        exitDef = oldDef;
      }
    }

    // Walk through the basic block to apply updates to non-phis (we'll handle
    // the phis later).  PERF: this could be easily optimized by a) checking to
    // see if there are any uses in this block and b) visiting only the uses
    for (BasicBlock::iterator itr = currentBB->getFirstNonPHI(),
                              end = currentBB->end();
         itr != end; itr++) {
      Instruction *inst = &*itr;
      if (exitDef) {
        inst->replaceUsesOfWith(oldDef, exitDef);
      } else {
        for (User *User : oldDef->users()) {
          Instruction *UserInst = cast<Instruction>(User);
          assert(UserInst != inst && "encountered a use without a valid def!");
        }
      }

      if (inst == oldDef) {
        // We encountered the original definition
        exitDef = oldDef;
      }

      if (isStatepoint(inst)) {
        // Find the relocated value after the safepoint

        // If we didn't find the value relocated at the safepoint, it wasn't
        // live across the safepoint.  There can be no uses reachable from her
        // and a NULL will be assigned to it.
        exitDef = findRelocateValueAtSP(inst, exitDef);
      }
    }

    if (TraceLSP) {
      errs() << "[TraceRelocations] leaving %" << currentBB->getName()
             << " with value %" << (exitDef ? exitDef->getName() : "NULL")
             << "\n";
    }
    assert(seen.find(currentBB) == seen.end() && "we're overwriting!");
    seen[currentBB] = exitDef;

    for (succ_iterator PI = succ_begin(currentBB), E = succ_end(currentBB);
         PI != E; ++PI) {
      BasicBlock *Succ = *PI;
      // optimization to avoid adding redundant work to worklist
      if (seen.find(Succ) != seen.end()) {
        // we've seen this before, no need to revisit
        if (TraceLSP) {
          errs() << "Skipping %" << Succ->getName() << " as previously seen\n";
        }
        continue;
      }
      // The exiting def of this block is the entry def of the successors
      frontier.push_back(make_pair(Succ, exitDef));
    }
  }

  if (TraceLSP) {
    errs() << "[TraceRelocations] map<BasicBlock *, Instruction *> seen == \n";
    for (const std::pair<BasicBlock *, Value *> &I : seen) {
      errs() << "\tseen[%" << I.first->getName() << "] = %"
             << (I.second ? I.second->getName() : "NULL") << "\n";
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
