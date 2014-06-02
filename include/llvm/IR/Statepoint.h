
#ifndef __LLVM_IR_STATEPOINT_H__
#define __LLVM_IR_STATEPOINT_H__

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

  bool isStatepoint(const ImmutableCallSite& CS);
  bool isStatepoint(const Instruction* inst);
  bool isStatepoint(const Instruction& inst);

  bool isGCRelocate(const Instruction* inst);
  bool isGCRelocate(const ImmutableCallSite& CS);

  bool isGCResult(const Instruction* inst);
  bool isGCResult(const ImmutableCallSite& CS);

// A helper class for access statepoint arguments.  Note, this does
// NOT take ownership of the IR and should be used only as a local stack object.
class StatepointOperands {
  ImmutableCallSite  _statepoint;
  void *operator new(size_t, unsigned) LLVM_DELETED_FUNCTION;
  void *operator new(size_t s) LLVM_DELETED_FUNCTION;

 public:
  StatepointOperands(const Instruction* inst)
    : _statepoint(inst) {
    assert( isStatepoint(inst) );
  }
  StatepointOperands(CallSite CS)
    : _statepoint(CS) {
    assert( isStatepoint(CS) );
  }

  const Value *actualCallee() {
    return _statepoint.getArgument(0);
  }
  int numCallArgs() {
    return cast<ConstantInt>(_statepoint.getArgument(1))->getZExtValue();
  }
  int bci() {
    return cast<ConstantInt>(_statepoint.getArgument(3))->getZExtValue();
  }
  int numJavaStackArgs() {
    return cast<ConstantInt>(_statepoint.getArgument(4))->getZExtValue();
  }
  int numJavaLocalArgs() {
    return cast<ConstantInt>(_statepoint.getArgument(5))->getZExtValue();
  }
  int numJavaMonitorArgs() {
    return cast<ConstantInt>(_statepoint.getArgument(6))->getZExtValue();
  }

  ImmutableCallSite::arg_iterator call_args_begin() {
    int offset = 7;
    assert( offset <= std::distance(_statepoint.arg_begin(), _statepoint.arg_end()) );
    return _statepoint.arg_begin() + offset;
  }
  ImmutableCallSite::arg_iterator  call_args_end() {
    int offset = 7 + numCallArgs();
    assert( offset <= std::distance(_statepoint.arg_begin(), _statepoint.arg_end()) );
    return _statepoint.arg_begin() + offset;
  }

  ImmutableCallSite::arg_iterator deopt_args_begin() {
    return call_args_end();
  }
  ImmutableCallSite::arg_iterator  deopt_args_end() {
    int offset = 7 + numCallArgs() + numJavaStackArgs() + numJavaLocalArgs() + numJavaMonitorArgs();
    assert( offset <= std::distance(_statepoint.arg_begin(), _statepoint.arg_end()) );
    return _statepoint.arg_begin() + offset;
  }
  
  ImmutableCallSite::arg_iterator  gc_args_begin() {
    return deopt_args_end();
  }
  ImmutableCallSite::arg_iterator  gc_args_end() {
    return _statepoint.arg_end();
  }

  // Return the gc_relocate which relocates this pointer
  const Instruction* getRelocatedPtr(Value* ptr);

#ifndef NDEBUG
  void verify() {
    // The internal asserts in the iterator accessors do the rest.
    assert(std::distance(call_args_end(), deopt_args_end()) >= 0);
    assert(std::distance(deopt_args_end(), gc_args_end()) >= 0);
  }
#endif
};
 
class GCRelocateOperands {
  ImmutableCallSite  _relocate;
 public:
  GCRelocateOperands(const Instruction* inst)
    : _relocate(inst) {
    assert( isGCRelocate(inst) );
  }
  GCRelocateOperands(CallSite CS)
    : _relocate(CS) {
    assert( isGCRelocate(CS) );
  }

  const Instruction* statepoint() {
    return cast<Instruction>(_relocate.getArgument(0));
  }
  int basePtrIndex() {
    return cast<ConstantInt>(_relocate.getArgument(1))->getZExtValue();
  }
  int derivedPtrIndex() {
    return cast<ConstantInt>(_relocate.getArgument(2))->getZExtValue();
  }
  const Value* basePtr() {
    ImmutableCallSite CS(statepoint());
    return *(CS.arg_begin() + basePtrIndex());
  }
  const Value* derivedPtr() {
    ImmutableCallSite CS(statepoint());
    return *(CS.arg_begin() + derivedPtrIndex());
  }
};
/// Returns true if and only if the type specified is a
/// pointer to a GC'd object which must be included in
/// barriers and safepoints.
bool isGCPointerType(llvm::Type* Ty);
}
#endif
