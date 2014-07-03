
#ifndef LLVM_IR_SAFEPOINT_IR_VERIFIER
#define LLVM_IR_SAFEPOINT_IR_VERIFIER

namespace llvm {

  class Function;
  class FunctionPass;

  void verifySafepointIR(Function& F);

  FunctionPass* createSafepointIRVerifierPass();
}

#endif //LLVM_IR_SAFEPOINT_IR_VERIFIER
