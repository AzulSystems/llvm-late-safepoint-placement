// TODO: copyright header

#define DEBUG_TYPE "remove-phantom-arg"

#include "llvm/IR/DebugInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace {
struct RemovePhantomArg : public ModulePass {
  // Map each LLVM function to corresponding metadata with debug info. If
  // the function is replaced with another one, we should patch the pointer
  // to LLVM function in metadata.
  // As the code generation for module is finished (and DIBuilder is
  // finalized) we assume that subprogram descriptors won't be changed, and
  // they are stored in map for short duration anyway.
  DenseMap<const Function *, DISubprogram> FunctionDIs;

  static char ID; // Pass identification, replacement for typeid

  RemovePhantomArg() : ModulePass(ID) {
    initializeRemovePhantomArgPass(*PassRegistry::getPassRegistry());
  }
  virtual bool runOnModule(Module &M) {
    bool modified = false;

    // Collect debug info descriptors for functions.
    FunctionDIs = makeSubprogramMap(M);

    for (Module::iterator I = M.begin(), E = M.end(); I != E; ) {
      Function* F = I++;

      // we don't support varArg
      if (!F->getFunctionType()->isVarArg())
        modified |= removePhantomArgument(F);
    }
    return modified;
  }

private:
  bool removePhantomArgument(Function* F);
  bool shouldGetSafepoints(Function &F);
  bool isFunctionAttrTrue(Function &F, const char *attr);
};
}

bool RemovePhantomArg::shouldGetSafepoints(Function &F) {
  return isFunctionAttrTrue(F, "gc-add-backedge-safepoints") ||
      isFunctionAttrTrue(F, "gc-add-call-safepoints") ||
      isFunctionAttrTrue(F, "gc-add-entry_safepoints");
}

bool RemovePhantomArg::isFunctionAttrTrue(Function &F,
                                                    const char *attr) {
  return F.getFnAttribute(attr).getValueAsString().equals("true");
}


bool RemovePhantomArg::removePhantomArgument(Function* F) {
  assert(!F->getFunctionType()->isVarArg() && "Function is varargs!");
  // if this is a callee function that get inlined, we don't
  // need to change it because it will be dropped before
  // codegen anyway
  if (F->hasFnAttribute("is-inlined-callee")) return false;

  // if a function should get safepoint then it needs vmstate
  // and should have the phantom arg
  if (!shouldGetSafepoints(*F)) return false;

  // Start by computing a new prototype for the function, which is the same as
  // the old function, but has one less argument.
  FunctionType *FTy = F->getFunctionType();
  std::vector<Type*> Params;

  // Set up to build a new list of parameter attributes.
  SmallVector<AttributeSet, 8> AttributesVec;
  const AttributeSet &PAL = F->getAttributes();

  // Construct the new parameter list from non-phantom arguments. Also
  // construct a new set of parameter attributes to correspond. Skip the
  // first parameter attribute, since that belongs to the return value.
  unsigned i = 1;
  Function::arg_iterator I = F->arg_begin();
  llvm::Argument* phantomArg = I++;
  assert(phantomArg->getType()->isIntegerTy(32) && "First arg must be int32");
  for (Function::arg_iterator E = F->arg_end();
       I != E; ++I, ++i) {
    Params.push_back(I->getType());

    // Get the original parameter attributes (skipping the first one, that is
    // for the return value.
    if (PAL.hasAttributes(i + 1)) {
      AttrBuilder B(PAL, i + 1);
      AttributesVec.
        push_back(AttributeSet::get(F->getContext(), Params.size(), B));
    }
  }

  // Find out the new return value.
  Type *NRetTy = FTy->getReturnType();

  // The existing function return attributes.
  AttributeSet RAttrs = PAL.getRetAttributes();

  if (RAttrs.hasAttributes(AttributeSet::ReturnIndex))
    AttributesVec.push_back(AttributeSet::get(NRetTy->getContext(), RAttrs));

  if (PAL.hasAttributes(AttributeSet::FunctionIndex))
    AttributesVec.push_back(AttributeSet::get(F->getContext(),
                                              PAL.getFnAttributes()));

  // Reconstruct the AttributesList based on the vector we constructed.
  AttributeSet NewPAL = AttributeSet::get(F->getContext(), AttributesVec);

  // Create the new function type based on the recomputed parameters.
  FunctionType *NFTy = FunctionType::get(NRetTy, Params, false);

  assert(NFTy != FTy && "They should not be the same");

  // Create the new function body and insert it into the module...
  Function *NF = Function::Create(NFTy, F->getLinkage());
  NF->copyAttributesFrom(F);
  NF->setAttributes(NewPAL);
  // Insert the new function before the old function, so we won't be processing
  // it again.
  F->getParent()->getFunctionList().insert(F, NF);
  NF->takeName(F);

  // Since we have now created the new function, splice the body of the old
  // function right into the new function, leaving the old rotting hulk of the
  // function empty.
  NF->getBasicBlockList().splice(NF->begin(), F->getBasicBlockList());

  // Loop over the argument list, transferring uses of the old arguments over to
  // the new arguments, also transferring over the names as well.
  for (Function::arg_iterator I = ++F->arg_begin(), E = F->arg_end(),
       I2 = NF->arg_begin(); I != E; ++I, ++I2) {
      I->replaceAllUsesWith(I2);
      I2->takeName(I);
  }

  // Patch the pointer to LLVM function in debug info descriptor.
  auto DI = FunctionDIs.find(F);
  if (DI != FunctionDIs.end())
    DI->second.replaceFunction(NF);

  assert(F->use_empty() && "Function should have no directly use");

  // Now that the old function is dead, delete it.
  F->eraseFromParent();

  return true;
}

char RemovePhantomArg::ID = 0;

ModulePass *llvm::createRemovePhantomArgPass() {
  return new RemovePhantomArg();
}

INITIALIZE_PASS_BEGIN(RemovePhantomArg,
                "remove-phantom-argument", "Remove phantom argument", false, false)
INITIALIZE_PASS_END(RemovePhantomArg,
                    "remove-phantom-argument", "Remove phantom argument", false, false)
