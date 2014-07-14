
#ifndef __LLVM_IR_STATEPOINT_H__
#define __LLVM_IR_STATEPOINT_H__

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/JVMState.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

bool isStatepoint(const ImmutableCallSite &CS);
bool isStatepoint(const Instruction *inst);
bool isStatepoint(const Instruction &inst);

bool isGCRelocate(const Instruction *inst);
bool isGCRelocate(const ImmutableCallSite &CS);

bool isGCResult(const Instruction *inst);
bool isGCResult(const ImmutableCallSite &CS);

template <typename InstructionTy, typename ValueTy, typename CallSiteTy>
class StatepointBase {
  CallSiteTy callSite;
  void *operator new(size_t, unsigned) LLVM_DELETED_FUNCTION;
  void *operator new(size_t s) LLVM_DELETED_FUNCTION;

protected:
  explicit StatepointBase(InstructionTy *I) : callSite(I) {
    assert(isStatepoint(I));
  }
  explicit StatepointBase(CallSiteTy CS) : callSite(CS) {
    assert(isStatepoint(CS));
  }

public:
  ValueTy *actualCallee() { return callSite.getArgument(0); }
  int numCallArgs() {
    return cast<ConstantInt>(callSite.getArgument(1))->getZExtValue();
  }
  int bci() {
    return cast<ConstantInt>(callSite.getArgument(3))->getZExtValue();
  }
  int numJavaStackElements() {
    return cast<ConstantInt>(callSite.getArgument(4))->getZExtValue();
  }
  int numJavaLocals() {
    return cast<ConstantInt>(callSite.getArgument(5))->getZExtValue();
  }
  int numJavaMonitors() {
    return cast<ConstantInt>(callSite.getArgument(6))->getZExtValue();
  }

  ValueTy *javaStackElementAt(int i) {
    assert(i >= 0 && i < numJavaStackElements() && "out of bounds!");
    return *(vm_state_stack_begin() + 2 * i + 1);
  }
  OpaqueJVMTypeID javaStackElementTypeAt(int i) {
    assert(i >= 0 && i < numJavaStackElements() && "out of bounds!");
    int tyInt =
        cast<ConstantInt>(*(vm_state_stack_begin() + 2 * i))->getZExtValue();
    return OpaqueJVMTypeID(tyInt);
  }

  ValueTy *javaLocalAt(int i) {
    assert(i >= 0 && i < numJavaLocals() && "out of bounds!");
    return *(vm_state_begin() + 2 * numJavaStackElements() + 2 * i + 1);
  }
  OpaqueJVMTypeID javaLocalTypeAt(int i) {
    assert(i >= 0 && i < numJavaLocals() && "out of bounds!");
    int tyInt =
        cast<ConstantInt>(*(vm_state_locals_begin() + 2 * i))->getZExtValue();
    return OpaqueJVMTypeID(tyInt);
  }

  ValueTy *javaMonitorAt(int i) {
    assert(i >= 0 && i < numJavaMonitors() && "out of bounds!");
    return *(vm_state_begin() + 2 * numJavaStackElements() +
             2 * numJavaLocals() + i);
  }

  typename CallSiteTy::arg_iterator call_args_begin() {
    int offset = 7;
    assert(offset <= callSite.arg_size());
    return callSite.arg_begin() + offset;
  }
  typename CallSiteTy::arg_iterator call_args_end() {
    int offset = 7 + numCallArgs();
    assert(offset <= callSite.arg_size());
    return callSite.arg_begin() + offset;
  }

  typename CallSiteTy::arg_iterator vm_state_begin() { return call_args_end(); }
  typename CallSiteTy::arg_iterator vm_state_end() {
    int offset = 7 + numCallArgs() + 2 * numJavaStackElements() +
                 2 * numJavaLocals() + numJavaMonitors();
    assert(offset <= callSite.arg_size());
    return callSite.arg_begin() + offset;
  }

  typename CallSiteTy::arg_iterator vm_state_stack_begin() {
    return vm_state_begin();
  }
  typename CallSiteTy::arg_iterator vm_state_stack_end() {
    return vm_state_stack_begin() + 2 * numJavaStackElements();
  }

  typename CallSiteTy::arg_iterator vm_state_locals_begin() {
    return vm_state_stack_end();
  }
  typename CallSiteTy::arg_iterator vm_state_locals_end() {
    return vm_state_locals_begin() + 2 * numJavaLocals();
  }

  typename CallSiteTy::arg_iterator vm_state_monitors_begin() {
    return vm_state_locals_end();
  }
  typename CallSiteTy::arg_iterator vm_state_monitors_end() {
    return vm_state_end();
  }

  typename CallSiteTy::arg_iterator gc_args_begin() { return vm_state_end(); }
  typename CallSiteTy::arg_iterator gc_args_end() { return callSite.arg_end(); }

#ifndef NDEBUG
  void verify() {
    // The internal asserts in the iterator accessors do the rest.
    (void)call_args_begin();
    (void)call_args_end();
    (void)vm_state_begin();
    (void)vm_state_end();
    (void)gc_args_begin();
    (void)gc_args_end();
  }
#endif
};

class ImmutableStatepoint
    : public StatepointBase<const Instruction, const Value, ImmutableCallSite> {
  typedef StatepointBase<const Instruction, const Value, ImmutableCallSite>
  Base;

public:
  explicit ImmutableStatepoint(const Instruction *I) : Base(I) {}
  explicit ImmutableStatepoint(ImmutableCallSite CS) : Base(CS) {}
};

class Statepoint : public StatepointBase<Instruction, Value, CallSite> {
  typedef StatepointBase<Instruction, Value, CallSite> Base;

public:
  explicit Statepoint(Instruction *I) : Base(I) {}
  explicit Statepoint(CallSite CS) : Base(CS) {}
};

class GCRelocateOperands {
  ImmutableCallSite _relocate;

public:
  GCRelocateOperands(const Instruction *inst) : _relocate(inst) {
    assert(isGCRelocate(inst));
  }
  GCRelocateOperands(CallSite CS) : _relocate(CS) { assert(isGCRelocate(CS)); }

  const Instruction *statepoint() {
    return cast<Instruction>(_relocate.getArgument(0));
  }
  int basePtrIndex() {
    return cast<ConstantInt>(_relocate.getArgument(1))->getZExtValue();
  }
  int derivedPtrIndex() {
    return cast<ConstantInt>(_relocate.getArgument(2))->getZExtValue();
  }
  const Value *basePtr() {
    ImmutableCallSite CS(statepoint());
    return *(CS.arg_begin() + basePtrIndex());
  }
  const Value *derivedPtr() {
    ImmutableCallSite CS(statepoint());
    return *(CS.arg_begin() + derivedPtrIndex());
  }
};
/// Returns true if and only if the type specified is a
/// pointer to a GC'd object which must be included in
/// barriers and safepoints.
bool isGCPointerType(llvm::Type *Ty);
}
#endif
